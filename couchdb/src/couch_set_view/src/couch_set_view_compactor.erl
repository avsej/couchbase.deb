% Licensed under the Apache License, Version 2.0 (the "License"); you may not
% use this file except in compliance with the License. You may obtain a copy of
% the License at
%
%   http://www.apache.org/licenses/LICENSE-2.0
%
% Unless required by applicable law or agreed to in writing, software
% distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
% WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
% License for the specific language governing permissions and limitations under
% the License.

-module(couch_set_view_compactor).

-include ("couch_db.hrl").
-include_lib("couch_set_view/include/couch_set_view.hrl").

-export([start_compact/2, start_compact/3, start_compact/4,
         cancel_compact/2, cancel_compact/3]).

-define(SORTED_CHUNK_SIZE, 1024 * 1024).

-record(acc, {
   changes = 0,
   total_changes
}).

-spec start_compact(binary(), binary()) -> {'ok', pid()} |
                                           {'error', 'initial_build'}.
start_compact(SetName, DDocId) ->
    start_compact(SetName, DDocId, main).

-spec start_compact(binary(), binary(), set_view_group_type()) ->
                           {'ok', pid()} |
                           {'error', 'initial_build'}.
start_compact(SetName, DDocId, Type) ->
    start_compact(SetName, DDocId, Type, []).

-spec start_compact(binary(), binary(),
                    set_view_group_type(), list()) -> {'ok', pid()} |
                                                      {'error', 'initial_build'}.
start_compact(SetName, DDocId, Type, UserTaskStatus) ->
    {ok, Pid} = get_group_pid(SetName, DDocId, Type),
    gen_server:call(Pid, {start_compact, mk_compact_group(UserTaskStatus)}, infinity).



-spec cancel_compact(binary(), binary()) -> 'ok'.
cancel_compact(SetName, DDocId) ->
    cancel_compact(SetName, DDocId, main).

-spec cancel_compact(binary(), binary(), set_view_group_type()) -> 'ok'.
cancel_compact(SetName, DDocId, Type) ->
    {ok, Pid} = get_group_pid(SetName, DDocId, Type),
    gen_server:call(Pid, cancel_compact).


%%=============================================================================
%% internal functions
%%=============================================================================

-spec mk_compact_group(list()) -> CompactGroupFun
  when CompactGroupFun :: fun((#set_view_group{},
                               #set_view_group{},
                               string(),
                               pid() | 'nil',
                               pid()) -> no_return()).
mk_compact_group(UserStatus) ->
    fun(Group, EmptyGroup, TmpDir, UpdaterPid, Owner) ->
        compact_group(Group, EmptyGroup, TmpDir, UpdaterPid, Owner, UserStatus)
    end.

-spec compact_group(#set_view_group{},
                    #set_view_group{},
                    string(),
                    pid() | 'nil',
                    pid(),
                    list()) -> no_return().
compact_group(Group0, EmptyGroup, TmpDir, UpdaterPid, Owner, UserStatus) ->
    #set_view_group{
        set_name = SetName,
        type = Type
    } = Group0,

    case is_pid(UpdaterPid) of
    true ->
        MonRef = erlang:monitor(process, UpdaterPid),
        Ref = make_ref(),
        UpdaterPid ! {compactor_started, self(), Ref},
        Group = receive
        {Ref, {ok, Group2}} ->
            erlang:demonitor(MonRef, [flush]),
            Group2;
        {'DOWN', MonRef, _, _, {updater_finished, UpResult}} ->
            UpResult#set_view_updater_result.group;
        {'DOWN', MonRef, _, _, noproc} ->
            % updater just finished
            {ok, Group2} = gen_server:call(Owner, request_group, infinity),
            Group2;
        {'DOWN', MonRef, _, _, Reason2} ->
            exit({updater_died, Reason2})
        end;
    false ->
        Group = Group0
    end,

    #set_view_group{
        id_btree = IdBtree,
        views = Views,
        index_header = Header,
        sig = GroupSig
    } = Group,
    StartTime = os:timestamp(),

    #set_view_group{
        id_btree = EmptyIdBtree,
        views = EmptyViews,
        fd = Fd
    } = EmptyGroup,

    TotalChanges = total_kv_count(Group),
    Acc0 = #acc{total_changes = TotalChanges},

    DDocIds = couch_set_view_util:get_ddoc_ids_with_sig(SetName, Group),

    Status = merge_statuses(UserStatus, [
        {type, view_compaction},
        {set, SetName},
        {signature, ?l2b(couch_util:to_hex(GroupSig))},
        {design_documents, DDocIds},
        {changes_done, 0},
        {total_changes, TotalChanges},
        {indexer_type, Type},
        {progress, case TotalChanges of 0 -> 100; _ -> 0 end}
    ]),

    couch_task_status:add_task(Status),
    couch_task_status:set_update_frequency(5000),

    ok = couch_set_view_util:open_raw_read_fd(Group),

    BeforeKVWriteFun = fun(KV, Acc) ->
        {KV, update_task(Acc, 1)}
    end,

    FilterFun = case ?set_cbitmask(Group) of
    0 ->
        fun(_Kv) -> true end;
    _ ->
        fun({_Key, <<PartId:16, _/binary>>}) ->
            ((1 bsl PartId) band ?set_cbitmask(Group)) =:= 0
        end
    end,

    {ok, NewIdBtreeRoot, Acc1} = couch_btree_copy:copy(
        IdBtree, Fd,
        [{before_kv_write, {BeforeKVWriteFun, Acc0}}, {filter, FilterFun}]),
    NewIdBtree = EmptyIdBtree#btree{root = NewIdBtreeRoot},

    {NewViews, _} = lists:mapfoldl(fun({View, EmptyView}, Acc) ->
        compact_view(Fd, View, EmptyView, FilterFun, Acc)
    end, Acc1, lists:zip(Views, EmptyViews)),

    ok = couch_set_view_util:close_raw_read_fd(Group),

    NewGroup = EmptyGroup#set_view_group{
        id_btree = NewIdBtree,
        views = NewViews,
        index_header = Header#set_view_index_header{
            cbitmask = 0,
            id_btree_state = couch_btree:get_state(NewIdBtree),
            view_states = [couch_btree:get_state(V#set_view.btree) || V <- NewViews]
        }
    },

    CleanupKVCount = TotalChanges - total_kv_count(NewGroup),
    CompactResult = #set_view_compactor_result{
        group = NewGroup,
        cleanup_kv_count = CleanupKVCount
    },
    maybe_retry_compact(CompactResult, StartTime, TmpDir, Owner, 1).

merge_statuses(UserStatus, OurStatus) ->
    UserStatus0 =
        lists:filter(
            fun ({Key, _}) ->
                not lists:keymember(Key, 1, OurStatus)
            end, UserStatus),
    UserStatus0 ++ OurStatus.

maybe_retry_compact(CompactResult0, StartTime, TmpDir, Owner, Retries) ->
    NewGroup = CompactResult0#set_view_compactor_result.group,
    #set_view_group{
        set_name = SetName,
        name = DDocId,
        type = Type,
        fd = Fd
    } = NewGroup,
    CompactResult = CompactResult0#set_view_compactor_result{
        compact_time = timer:now_diff(os:timestamp(), StartTime) / 1000000
    },
    % For compaction retry testing purposes
    receive
    pause ->
        receive unpause -> ok end
    after 0 ->
        ok
    end,
    ok = couch_file:flush(Fd),
    case gen_server:call(Owner, {compact_done, CompactResult}, infinity) of
    ok ->
        ok;
    {update, MissingCount} ->
        {ok, {LogFiles, NewSeqs}} = gen_server:call(
            Owner, compact_log_files, infinity),
        ?LOG_INFO("Compactor for set view `~s`, ~s group `~s`, "
                  "applying delta of ~p changes (retry number ~p)",
                  [SetName, Type, DDocId, MissingCount, Retries]),
        [TotalChanges] = couch_task_status:get([total_changes]),
        TotalChanges2 = TotalChanges + MissingCount,
        couch_task_status:update([
            {total_changes, TotalChanges2},
            {changes_done, TotalChanges},
            {progress, (TotalChanges * 100) div TotalChanges2},
            {retry_number, Retries}
        ]),
        ok = couch_set_view_util:open_raw_read_fd(NewGroup),
        NewGroup2 = apply_log(NewGroup, LogFiles, NewSeqs, TmpDir),
        ok = couch_set_view_util:close_raw_read_fd(NewGroup),
        CompactResult2 = CompactResult0#set_view_compactor_result{
            group = NewGroup2
        },
        maybe_retry_compact(CompactResult2, StartTime, TmpDir, Owner, Retries + 1)
    end.


get_group_pid(SetName, DDocId, main) ->
    Pid = couch_set_view:get_group_pid(SetName, DDocId),
    {ok, Pid};
get_group_pid(SetName, DDocId, replica) ->
    Pid = couch_set_view:get_group_pid(SetName, DDocId),
    {ok, Group} = gen_server:call(Pid, request_group, infinity),
    case is_pid(Group#set_view_group.replica_pid) of
    true ->
        {ok, Group#set_view_group.replica_pid};
    false ->
        no_replica_group_found
    end.


compact_view(Fd, View, #set_view{btree = ViewBtree} = EmptyView, FilterFun, Acc0) ->
    BeforeKVWriteFun = fun(Item, Acc) ->
        {Item, update_task(Acc, 1)}
    end,

    couch_set_view_mapreduce:start_reduce_context(View),
    {ok, NewBtreeRoot, Acc2} = couch_btree_copy:copy(
        View#set_view.btree, Fd,
        [{before_kv_write, {BeforeKVWriteFun, Acc0}}, {filter, FilterFun}]),
    couch_set_view_mapreduce:end_reduce_context(View),

    ViewBtree2 = ViewBtree#btree{root = NewBtreeRoot},
    NewView = EmptyView#set_view{
        btree = ViewBtree2
    },
    {NewView, Acc2}.


update_task(#acc{total_changes = 0} = Acc, _ChangesInc) ->
    Acc;
update_task(#acc{changes = Changes, total_changes = Total} = Acc, ChangesInc) ->
    Changes2 = Changes + ChangesInc,
    case (Changes2 rem 10000) == 0 of
    true ->
        couch_task_status:update([
            {changes_done, Changes2},
            {progress, (Changes2 * 100) div Total}
        ]);
    false ->
        ok
    end,
    Acc#acc{changes = Changes2}.


total_kv_count(#set_view_group{id_btree = IdBtree, views = Views}) ->
    {ok, <<IdCount:40, _/binary>>} = couch_btree:full_reduce(IdBtree),
    lists:foldl(
        fun(#set_view{btree = Bt} = View, Acc) ->
            couch_set_view_mapreduce:start_reduce_context(View),
            {ok, <<Count:40, _/binary>>} = couch_btree:full_reduce(Bt),
            couch_set_view_mapreduce:end_reduce_context(View),
            Acc + Count
        end,
        IdCount, Views).


apply_log(Group, LogFiles, NewSeqs, TmpDir) ->
    #set_view_group{
        id_btree = IdBtree,
        index_header = Header
    } = Group,

    [IdLogFiles | ViewLogFiles] = LogFiles,
    IdMergeFile = merge_files(IdLogFiles, IdBtree, TmpDir),
    {ok, NewIdBtree, _, _} = couch_set_view_updater_helper:update_btree(
        IdBtree, IdMergeFile, ?SORTED_CHUNK_SIZE),
    ok = file2:delete(IdMergeFile),

    NewViews = lists:zipwith(
        fun(#set_view{btree = Bt} = V, Files) ->
           MergeFile = merge_files(Files, Bt, TmpDir),
           {ok, NewBt, _, _} = couch_set_view_updater_helper:update_btree(
               Bt, MergeFile, ?SORTED_CHUNK_SIZE),
           ok = file2:delete(MergeFile),
           V#set_view{btree = NewBt}
        end,
        Group#set_view_group.views, ViewLogFiles),

    Group#set_view_group{
        id_btree = NewIdBtree,
        views = NewViews,
        index_header = Header#set_view_index_header{
            seqs = NewSeqs,
            id_btree_state = couch_btree:get_state(NewIdBtree),
            view_states = [couch_btree:get_state(V#set_view.btree) || V <- NewViews]
        }
    }.


merge_files([F], _Bt, _TmpDir) ->
    F;
merge_files(Files, #btree{less = Less}, TmpDir) ->
    NewFile = couch_set_view_util:new_sort_file_path(TmpDir, compactor),
    SortOptions = [
        {order, couch_set_view_updater_helper:batch_sort_fun(Less)},
        {tmpdir, TmpDir},
        {format, binary}
    ],
    ok = file_sorter_2:merge(Files, NewFile, SortOptions),
    ok = lists:foreach(fun(F) -> ok = file2:delete(F) end, Files),
    NewFile.
