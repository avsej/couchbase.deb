%% @author Northscale <info@northscale.com>
%% @copyright 2009 NorthScale, Inc.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%      http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
%% @doc Implementation of diagnostics web requests
-module(diag_handler).
-author('NorthScale <info@northscale.com>').

-include("ns_common.hrl").
-include("ns_log.hrl").
-include_lib("kernel/include/file.hrl").

-export([do_diag_per_node/0, do_diag_per_node_binary/0,
         handle_diag/1,
         handle_sasl_logs/1, handle_sasl_logs/2,
         arm_timeout/2, arm_timeout/1, disarm_timeout/1,
         grab_process_info/1, manifest/0,
         grab_all_tap_and_checkpoint_stats/0,
         grab_all_tap_and_checkpoint_stats/1,
         log_all_tap_and_checkpoint_stats/0,
         diagnosing_timeouts/1]).

diag_filter_out_config_password_list([], UnchangedMarker) ->
    UnchangedMarker;
diag_filter_out_config_password_list([X | Rest], UnchangedMarker) ->
    NewX = diag_filter_out_config_password_rec(X, UnchangedMarker),
    case diag_filter_out_config_password_list(Rest, UnchangedMarker) of
        UnchangedMarker ->
            case NewX of
                UnchangedMarker -> UnchangedMarker;
                _ -> [NewX | Rest]
            end;
        NewRest -> [case NewX of
                        UnchangedMarker -> X;
                        _ -> NewX
                    end | NewRest]
    end;
%% this handles improper list end
diag_filter_out_config_password_list(X, UnchangedMarker) ->
    diag_filter_out_config_password_list([X], UnchangedMarker).

diag_filter_out_config_password_rec(Config, UnchangedMarker) when is_tuple(Config) ->
    case Config of
        {password, _} ->
            {password, 'filtered-out'};
        {pass, _} ->
            {pass, 'filtered-out'};
        _ -> case diag_filter_out_config_password_rec(tuple_to_list(Config), UnchangedMarker) of
                 UnchangedMarker -> Config;
                 List -> list_to_tuple(List)
             end
    end;
diag_filter_out_config_password_rec(Config, UnchangedMarker) when is_list(Config) ->
    diag_filter_out_config_password_list(Config, UnchangedMarker);
diag_filter_out_config_password_rec(_Config, UnchangedMarker) -> UnchangedMarker.

diag_filter_out_config_password(Config) ->
    UnchangedMarker = make_ref(),
    case diag_filter_out_config_password_rec(Config, UnchangedMarker) of
        UnchangedMarker -> Config;
        NewConfig -> NewConfig
    end.

% Read the manifest.xml file
manifest() ->
    case file:read_file(filename:join(path_config:component_path(bin, ".."), "manifest.xml")) of
        {ok, C} ->
            string:tokens(binary_to_list(C), "\n");
        _ -> []
    end.

grab_process_info(Pid) ->
    PureInfo = erlang:process_info(Pid,
                                   [registered_name,
                                    status,
                                    initial_call,
                                    backtrace,
                                    error_handler,
                                    garbage_collection,
                                    heap_size,
                                    total_heap_size,
                                    links,
                                    memory,
                                    message_queue_len,
                                    reductions,
                                    trap_exit]),
    Backtrace = proplists:get_value(backtrace, PureInfo),
    NewBacktrace = [case erlang:size(X) of
                        L when L =< 90 ->
                            binary:copy(X);
                        _ -> binary:copy(binary:part(X, 1, 90))
                    end || X <- binary:split(Backtrace, <<"\n">>, [global])],
    lists:keyreplace(backtrace, 1, PureInfo, {backtrace, NewBacktrace}).

grab_all_tap_and_checkpoint_stats() ->
    grab_all_tap_and_checkpoint_stats(15000).

grab_all_tap_and_checkpoint_stats(Timeout) ->
    ActiveBuckets = ns_memcached:active_buckets(),
    ThisNodeBuckets = ns_bucket:node_bucket_names_of_type(node(), membase),
    InterestingBuckets = ordsets:intersection(lists:sort(ActiveBuckets),
                                              lists:sort(ThisNodeBuckets)),
    WorkItems = [{Bucket, Type} || Bucket <- InterestingBuckets,
                                   Type <- [<<"tap">>, <<"checkpoint">>]],
    Results = misc:parallel_map(
                fun ({Bucket, Type}) ->
                        {ok, _} = timer:kill_after(Timeout),
                        case ns_memcached:stats(Bucket, Type) of
                            {ok, V} -> V;
                            Crap -> Crap
                        end
                end, WorkItems, infinity),
    {WiB, WiT} = lists:unzip(WorkItems),
    lists:zip3(WiB, WiT, Results).

log_all_tap_and_checkpoint_stats() ->
    ?log_info("logging tap & checkpoint stats"),
    [begin
         ?log_info("~s:~s:~n~p",[Type, Bucket, Values])
     end || {Bucket, Type, Values} <- grab_all_tap_and_checkpoint_stats()],
    ?log_info("end of logging tap & checkpoint stats").

do_diag_per_node() ->
    ActiveBuckets = ns_memcached:active_buckets(),
    [{version, ns_info:version()},
     {manifest, manifest()},
     {config, diag_filter_out_config_password(ns_config:get_kv_list())},
     {basic_info, element(2, ns_info:basic_info())},
     {processes, grab_process_infos_loop(erlang:processes(), [])},
     {memory, memsup:get_memory_data()},
     {disk, ns_info:get_disk_data()},
     {active_tasks, capi_frontend:task_status_all()},
     {master_events, (catch master_activity_events_keeper:get_history())},
     {ns_server_stats, (catch system_stats_collector:get_ns_server_stats())},
     {active_buckets, ActiveBuckets},
     {tap_stats, (catch grab_all_tap_and_checkpoint_stats(4000))}].

do_diag_per_node_binary() ->
    ActiveBuckets = ns_memcached:active_buckets(),
    Diag = [{version, ns_info:version()},
            {manifest, manifest()},
            {config, diag_filter_out_config_password(ns_config:get_kv_list())},
            {basic_info, element(2, ns_info:basic_info())},
            {processes, grab_process_infos_loop(erlang:processes(), [])},
            {memory, memsup:get_memory_data()},
            {disk, ns_info:get_disk_data()},
            {active_tasks, capi_frontend:task_status_all()},
            {master_events, (catch master_activity_events_keeper:get_history_raw())},
            {ns_server_stats, (catch system_stats_collector:get_ns_server_stats())},
            {active_buckets, ActiveBuckets},
            {tap_stats, (catch grab_all_tap_and_checkpoint_stats(4000))}],
    term_to_binary(Diag).

grab_process_infos_loop([], Acc) ->
    Acc;
grab_process_infos_loop([P | RestPids], Acc) ->
    NewAcc = [{P, (catch grab_process_info(P))} | Acc],
    grab_process_infos_loop(RestPids, NewAcc).

diag_format_timestamp(EpochMilliseconds) ->
    SecondsRaw = trunc(EpochMilliseconds/1000),
    AsNow = {SecondsRaw div 1000000, SecondsRaw rem 1000000, 0},
    %% not sure about local time here
    {{YYYY, MM, DD}, {Hour, Min, Sec}} = calendar:now_to_local_time(AsNow),
    io_lib:format("~4.4.0w-~2.2.0w-~2.2.0w ~2.2.0w:~2.2.0w:~2.2.0w.~3.3.0w",
                  [YYYY, MM, DD, Hour, Min, Sec, EpochMilliseconds rem 1000]).

generate_diag_filename() ->
    {{YYYY, MM, DD}, {Hour, Min, Sec}} = calendar:now_to_local_time(now()),
    io_lib:format("ns-diag-~4.4.0w~2.2.0w~2.2.0w~2.2.0w~2.2.0w~2.2.0w.txt",
                  [YYYY, MM, DD, Hour, Min, Sec]).

diag_format_log_entry(Type, Code, Module, Node, TStamp, ShortText, Text) ->
    FormattedTStamp = diag_format_timestamp(TStamp),
    io_lib:format("~s ~s:~B:~s:~s(~s) - ~s~n",
                  [FormattedTStamp, Module, Code, Type, ShortText, Node, Text]).

handle_diag(Req) ->
    Params = Req:parse_qs(),
    MaybeContDisp = case proplists:get_value("mode", Params) of
                        "view" -> [];
                        _ -> [{"Content-Disposition", "attachment; filename=" ++ generate_diag_filename()}]
                    end,
    case proplists:get_value("noLogs", Params, "0") of
        "0" ->
            do_handle_diag(Req, MaybeContDisp);
        _ ->
            Resp = handle_just_diag(Req, MaybeContDisp),
            Resp:write_chunk(<<>>)
    end.

grab_per_node_diag(Nodes) ->
    {Results0, BadNodes} = rpc:multicall(Nodes,
                                         ?MODULE, do_diag_per_node_binary, [], 45000),
    {OldNodeResults, Results1} =
        lists:partition(
          fun ({_Node, {badrpc, {'EXIT', R}}}) ->
                  misc:is_undef_exit(?MODULE, do_diag_per_node_binary, [], R);
              (_) ->
                  false
          end,
          lists:zip(lists:subtract(Nodes, BadNodes), Results0)),

    OldNodes = [N || {N, _} <- OldNodeResults],
    {Results2, BadResults} =
        lists:partition(
          fun ({_, {badrpc, _}}) ->
                  false;
              (_) ->
                  true
          end, Results1),

    Results = [{N, diag_failed} || N <- BadNodes] ++
        [{N, diag_failed} || {N, _} <- BadResults] ++ Results2,

    {Results, OldNodes}.

handle_just_diag(Req, Extra) ->
    Resp = Req:ok({"text/plain; charset=utf-8",
                   Extra ++
                       [{"Content-Type", "text/plain"}
                        | menelaus_util:server_header()],
                   chunked}),

    Nodes = ns_node_disco:nodes_actual_proper(),
    {Results, OldNodes} = grab_per_node_diag(Nodes),

    handle_per_node_just_diag(Resp, Results),
    handle_per_node_just_diag_old_nodes(Resp, OldNodes),

    Buckets = lists:sort(fun (A,B) -> element(1, A) =< element(1, B) end,
                         ns_bucket:get_buckets()),

    Infos = [["nodes_info = ~p", menelaus_web:build_nodes_info()],
             ["buckets = ~p", Buckets],
             ["logs:~n-------------------------------~n"]],

    [begin
         Text = io_lib:format(Fmt ++ "~n~n", Args),
         Resp:write_chunk(list_to_binary(Text))
     end || [Fmt | Args] <- Infos],

    lists:foreach(fun (#log_entry{node = Node,
                                  module = Module,
                                  code = Code,
                                  msg = Msg,
                                  args = Args,
                                  cat = Cat,
                                  tstamp = TStamp}) ->
                          try io_lib:format(Msg, Args) of
                              S ->
                                  CodeString = ns_log:code_string(Module, Code),
                                  Type = menelaus_alert:category_bin(Cat),
                                  TStampEpoch = misc:time_to_epoch_ms_int(TStamp),
                                  Resp:write_chunk(iolist_to_binary(diag_format_log_entry(Type, Code, Module, Node, TStampEpoch, CodeString, S)))
                          catch _:_ -> ok
                          end
                  end, lists:keysort(#log_entry.tstamp, ns_log:recent())),
    Resp.

write_chunk_format(Resp, Fmt, Args) ->
    Text = io_lib:format(Fmt, Args),
    Resp:write_chunk(list_to_binary(Text)).

handle_per_node_just_diag(_Resp, []) ->
    erlang:garbage_collect();
handle_per_node_just_diag(Resp, [{Node, DiagBinary} | Results]) ->
    erlang:garbage_collect(),

    Diag = binary_to_term(DiagBinary),
    do_handle_per_node_just_diag(Resp, Node, Diag),
    handle_per_node_just_diag(Resp, Results).

handle_per_node_just_diag_old_nodes(_Resp, []) ->
    erlang:garbage_collect();
handle_per_node_just_diag_old_nodes(Resp, [Node | Nodes]) ->
    erlang:garbage_collect(),

    PerNodeDiag = rpc:call(Node, ?MODULE, do_diag_per_node, [], 20000),
    do_handle_per_node_just_diag(Resp, Node, PerNodeDiag),
    handle_per_node_just_diag_old_nodes(Resp, Nodes).

do_handle_per_node_just_diag(Resp, Node, {badrpc, _}) ->
    do_handle_per_node_just_diag(Resp, Node, diag_failed);
do_handle_per_node_just_diag(Resp, Node, diag_failed) ->
    write_chunk_format(Resp, "per_node_diag(~p) = diag_failed~n~n~n", [Node]);
do_handle_per_node_just_diag(Resp, Node, PerNodeDiag) ->
    MasterEvents = proplists:get_value(master_events, PerNodeDiag, []),
    DiagNoMasterEvents = lists:keydelete(master_events, 1, PerNodeDiag),

    write_chunk_format(Resp, "master_events(~p) =~n", [Node]),
    lists:foreach(
      fun (Event0) ->
              Event = case is_binary(Event0) of
                          true ->
                              binary_to_term(Event0);
                          false ->
                              Event0
                      end,

              lists:foreach(
                fun (JSON) ->
                        write_chunk_format(Resp, "     ~p~n", [JSON])
                end, master_activity_events:event_to_jsons(Event))
      end, MasterEvents),
    Resp:write_chunk(<<"\n\n">>),

    do_handle_per_node_processes(Resp, Node, DiagNoMasterEvents).

do_handle_per_node_processes(Resp, Node, PerNodeDiag) ->
    erlang:garbage_collect(),

    Processes = proplists:get_value(processes, PerNodeDiag),
    DiagNoProcesses = lists:keydelete(processes, 1, PerNodeDiag),

    write_chunk_format(Resp, "per_node_processes(~p) =~n", [Node]),
    lists:foreach(
      fun (Process) ->
              write_chunk_format(Resp, "     ~p~n", [Process])
      end, Processes),
    Resp:write_chunk(<<"\n\n">>),

    do_continue_handling_per_node_just_diag(Resp, Node, DiagNoProcesses).

do_continue_handling_per_node_just_diag(Resp, Node, DiagNoProcesses) ->
    erlang:garbage_collect(),

    write_chunk_format(Resp, "per_node_diag(~p) =~n", [Node]),
    write_chunk_format(Resp, "     ~p~n", [DiagNoProcesses]),
    Resp:write_chunk(<<"\n\n">>).

do_handle_diag(Req, Extra) ->
    Resp = handle_just_diag(Req, Extra),

    Logs = [?DEBUG_LOG_FILENAME, ?STATS_LOG_FILENAME,
            ?DEFAULT_LOG_FILENAME, ?ERRORS_LOG_FILENAME,
            ?XDCR_LOG_FILENAME, ?COUCHDB_LOG_FILENAME,
            ?VIEWS_LOG_FILENAME, ?MAPREDUCE_ERRORS_LOG_FILENAME],

    lists:foreach(fun (Log) ->
                          handle_log(Resp, Log)
                  end, Logs),
    handle_memcached_logs(Resp),
    Resp:write_chunk(<<"">>).

handle_log(Resp, LogName) ->
    LogsHeader = io_lib:format("logs_node (~s):~n"
                               "-------------------------------~n", [LogName]),
    Resp:write_chunk(list_to_binary(LogsHeader)),
    ns_log_browser:stream_logs(LogName,
                               fun (Data) -> Resp:write_chunk(Data) end),
    Resp:write_chunk(<<"-------------------------------\n">>).


handle_memcached_logs(Resp) ->
    Config = ns_config:get(),
    Path = ns_config:search_node_prop(Config, memcached, log_path),
    Prefix = ns_config:search_node_prop(Config, memcached, log_prefix),

    {ok, AllFiles} = file:list_dir(Path),
    Logs = [filename:join(Path, F) || F <- AllFiles,
                                      string:str(F, Prefix) == 1],
    TsLogs = lists:map(fun(F) ->
                               case file:read_file_info(F) of
                                   {ok, Info} ->
                                       {F, Info#file_info.mtime};
                                   {error, enoent} ->
                                       deleted
                               end
                       end, Logs),

    Sorted = lists:keysort(2, lists:filter(fun (X) -> X /= deleted end, TsLogs)),

    Resp:write_chunk(<<"memcached logs:\n-------------------------------\n">>),

    lists:foreach(fun ({Log, _}) ->
                          handle_memcached_log(Resp, Log)
                  end, Sorted).

handle_memcached_log(Resp, Log) ->
    case file:open(Log, [raw, binary]) of
        {ok, File} ->
            do_handle_memcached_log(Resp, File);
        Error ->
            Resp:write_chunk(
              list_to_binary(io_lib:format("Could not open file ~s: ~p~n", [Log, Error])))
    end.

-define(CHUNK_SIZE, 65536).

do_handle_memcached_log(Resp, File) ->
    case file:read(File, ?CHUNK_SIZE) of
        eof ->
            ok;
        {ok, Data} ->
            Resp:write_chunk(Data)
    end.

handle_sasl_logs(LogName, Req) ->
    case ns_log_browser:log_exists(LogName) of
        true ->
            Resp = Req:ok({"text/plain; charset=utf-8",
                           menelaus_util:server_header(),
                           chunked}),
            handle_log(Resp, LogName),
            Resp:write_chunk(<<"">>);
        false ->
            Req:respond({404, menelaus_util:server_header(),
                         "Requested log file not found.\r\n"})
    end.

handle_sasl_logs(Req) ->
    handle_sasl_logs(?DEBUG_LOG_FILENAME, Req).

arm_timeout(Millis) ->
    arm_timeout(Millis,
                fun (Pid) ->
                        Info = (catch grab_process_info(Pid)),
                        ?log_error("slow process info:~n~p~n", [Info])
                end).

arm_timeout(Millis, Callback) ->
    Pid = self(),
    spawn_link(fun () ->
                       receive
                           done -> ok
                       after Millis ->
                               erlang:unlink(Pid),
                               Callback(Pid)
                       end,
                       erlang:unlink(Pid)
               end).

disarm_timeout(Pid) ->
    Pid ! done.

diagnosing_timeouts(Body) ->
    try Body()
    catch exit:{timeout, _} = X ->
            timeout_diag_logger:log_diagnostics(X),
            exit(X)
    end.
