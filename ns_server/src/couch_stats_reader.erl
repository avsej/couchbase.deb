%% @author Couchbase <info@couchbase.com>
%% @copyright 2011 Couchbase, Inc.
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

-module(couch_stats_reader).

-include_lib("eunit/include/eunit.hrl").

-include("couch_db.hrl").
-include("ns_common.hrl").
-include("ns_stats.hrl").

%% included to import #config{} record only
-include("ns_config.hrl").

-behaviour(gen_server).

-type per_ddoc_stats() :: {Sig::binary(),
                           DiskSize::integer(),
                           DataSize::integer(),
                           Accesses::integer()}.

-record(ns_server_couch_stats, {couch_docs_actual_disk_size,
                                couch_views_actual_disk_size,
                                couch_docs_disk_size,
                                couch_docs_data_size,
                                couch_views_disk_size,
                                couch_views_data_size,
                                per_ddoc_stats :: [per_ddoc_stats()]}).


%% API
-export([start_link/1, fetch_stats/1]).

%% gen_server callbacks
-export([init/1, handle_call/3, handle_cast/2,
         handle_info/2, terminate/2, code_change/3]).

-record(state, {bucket, last_ts, last_view_stats}).

%% Amount of time to wait between fetching stats
-define(SAMPLE_INTERVAL, 5000).


start_link(Bucket) ->
    gen_server:start_link({local, server(Bucket)}, ?MODULE, Bucket, []).

init(Bucket) ->
    {ok, BucketConfig} = ns_bucket:get_bucket(Bucket),
    case ns_bucket:bucket_type(BucketConfig) of
        membase ->
            self() ! refresh_stats,
            timer:send_interval(?SAMPLE_INTERVAL, refresh_stats);
        memcached ->
            ok
    end,
    ets:new(server(Bucket), [protected, named_table, set]),
    ets:insert(server(Bucket), {stuff, []}),
    {ok, #state{bucket=Bucket}}.

handle_call(_, _From, State) ->
    {reply, erlang:nif_error(unhandled), State}.

handle_cast(_Msg, State) ->
    {noreply, State}.

handle_info(refresh_stats, #state{bucket = Bucket,
                                  last_ts = LastTS,
                                  last_view_stats = LastViewStats} = State) ->
    misc:flush(refresh_stats),
    TS = misc:time_to_epoch_ms_int(os:timestamp()),

    Config = ns_config:get(),
    MinFileSize = ns_config:search_node_prop(Config,
                                             compaction_daemon, min_file_size, 131072),

    NewStats = grab_couch_stats(Bucket, Config, MinFileSize),
    {ProcessedSamples, NewLastViewStats} = parse_couch_stats(TS, NewStats, LastTS,
                                                             LastViewStats, MinFileSize),
    ets:insert(server(Bucket), {stuff, ProcessedSamples}),
    {noreply, State#state{last_view_stats = NewLastViewStats,
                          last_ts = TS}};

handle_info(_Msg, State) ->
    {noreply, State}.

terminate(_Reason, _State) ->
    ok.

code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

server(Bucket) ->
    list_to_atom(?MODULE_STRING ++ "-" ++ Bucket).

fetch_stats(Bucket) ->
    [{_, CouchStats}] = ets:lookup(server(Bucket), stuff),
    {ok, CouchStats}.

vbuckets_aggregation_loop(_Bucket, DiskSize, DataSize, _VBucketBinaries = []) ->
    {DiskSize, DataSize};
vbuckets_aggregation_loop(Bucket, DiskSize, DataSize, [VBucketBin | RestVBucketBinaries]) ->
    VBucket = iolist_to_binary([Bucket, <<"/">>, VBucketBin]),
    case couch_db:open_int(VBucket, []) of
        {ok, Db} ->
            {ok, Info} = try
                             couch_db:get_db_info(Db)
                         after
                             ok = couch_db:close(Db)
                         end,
            NewDiskSize = DiskSize + couch_util:get_value(disk_size, Info),
            NewDataSize = DataSize + couch_util:get_value(data_size, Info),
            vbuckets_aggregation_loop(Bucket, NewDiskSize, NewDataSize, RestVBucketBinaries);
        Why ->
            ?log_debug("Failed to open vbucket: ~s (~p). Ignoring", [VBucketBin, Why]),
            vbuckets_aggregation_loop(Bucket, DiskSize, DataSize, RestVBucketBinaries)
    end.

views_collection_loop_iteration(BinBucket, NameToStatsETS,  DDocId, MinFileSize) ->
    case (catch couch_set_view:get_group_data_size(BinBucket, DDocId)) of
        {ok, PList} ->
            {_, Signature} = lists:keyfind(signature, 1, PList),
            case ets:lookup(NameToStatsETS, Signature) of
                [] ->
                    {_, DiskSize} = lists:keyfind(disk_size, 1, PList),
                    {_, DataSize0} = lists:keyfind(data_size, 1, PList),
                    {_, Accesses} = lists:keyfind(accesses, 1, PList),

                    DataSize = maybe_adjust_data_size(DataSize0, DiskSize, MinFileSize),

                    ets:insert(NameToStatsETS, {Signature, DiskSize, DataSize, Accesses});
                _ ->
                    ok
            end;
        Why ->
            ?log_debug("Get group info (~s/~s) failed:~n~p", [BinBucket, DDocId, Why])
    end.

collect_view_stats(BinBucket, DDocIdList, MinFileSize) ->
    NameToStatsETS = ets:new(ok, []),
    try
        [views_collection_loop_iteration(BinBucket, NameToStatsETS, DDocId, MinFileSize)
         || DDocId <- DDocIdList],
        ets:tab2list(NameToStatsETS)
    after
        ets:delete(NameToStatsETS)
    end.

aggregate_view_stats_loop(DiskSize, DataSize, [{_, ThisDiskSize, ThisDataSize, _ThisAccesses} | RestViewStats]) ->
    aggregate_view_stats_loop(DiskSize + ThisDiskSize,
                              DataSize + ThisDataSize,
                              RestViewStats);
aggregate_view_stats_loop(DiskSize, DataSize, []) ->
    {DiskSize, DataSize}.

maybe_adjust_data_size(DataSize, DiskSize, MinFileSize) ->
    case DiskSize < MinFileSize of
        true ->
            DiskSize;
        false ->
            DataSize
    end.

-spec grab_couch_stats(bucket_name(), #config{}, integer()) -> #ns_server_couch_stats{}.
grab_couch_stats(Bucket, Config, MinFileSize) ->
    BinBucket = ?l2b(Bucket),
    {ok, BucketConfig} = ns_bucket:get_bucket(Bucket, Config),
    VBucketIds = ns_bucket:all_node_vbuckets(BucketConfig),
    {VBucketsDiskSize, VBucketsDataSize0} = vbuckets_aggregation_loop(BinBucket, 0, 0, [list_to_binary(integer_to_list(I)) || I <- VBucketIds]),

    VBucketsDataSize = maybe_adjust_data_size(VBucketsDataSize0, VBucketsDiskSize,
                                              MinFileSize * length(VBucketIds)),

    DDocIdList = capi_ddoc_replication_srv:fetch_ddoc_ids(BinBucket),
    ViewStats = collect_view_stats(BinBucket, DDocIdList, MinFileSize),
    {ViewsDiskSize, ViewsDataSize} = aggregate_view_stats_loop(0, 0, ViewStats),

    {ok, CouchDir} = ns_storage_conf:this_node_dbdir(),
    {ok, ViewRoot} = ns_storage_conf:this_node_ixdir(),

    DocsActualDiskSize = misc:dir_size(filename:join([CouchDir, Bucket])),
    ViewsActualDiskSize = misc:dir_size(couch_set_view:set_index_dir(ViewRoot, BinBucket)),

    #ns_server_couch_stats{couch_docs_actual_disk_size = DocsActualDiskSize,
                           couch_views_actual_disk_size = ViewsActualDiskSize,
                           couch_docs_disk_size = VBucketsDiskSize,
                           couch_docs_data_size = VBucketsDataSize,
                           couch_views_disk_size = ViewsDiskSize,
                           couch_views_data_size = ViewsDataSize,
                           per_ddoc_stats = lists:sort(ViewStats)}.

find_not_less_sig(Sig, [{CandidateSig, _, _, _} | RestViewStatsTuples] = VS) ->
    case CandidateSig < Sig of
        true ->
            find_not_less_sig(Sig, RestViewStatsTuples);
        false ->
            VS
    end;
find_not_less_sig(_Sig, []) ->
    [].

diff_view_accesses_loop(TSDelta, LastVS, [{Sig, DiskS, DataS, AccC} | VSRest]) ->
    NewLastVS = find_not_less_sig(Sig, LastVS),
    PrevAccC = case NewLastVS of
                   [{Sig, _, _, X} | _] -> X;
                   _ -> AccC
               end,
    Res0 = (AccC - PrevAccC) * 1000 / TSDelta,
    Res = case Res0 < 0 of
              true -> 0;
              _ -> Res0
          end,
    NewTuple = {Sig, DiskS, DataS, Res},
    [NewTuple | diff_view_accesses_loop(TSDelta, NewLastVS, VSRest)];
diff_view_accesses_loop(_TSDelta, _LastVS, [] = _ViewStats) ->
    [].

build_basic_couch_stats(CouchStats) ->
    #ns_server_couch_stats{couch_docs_actual_disk_size = DocsActualDiskSize,
                           couch_views_actual_disk_size = ViewsActualDiskSize,
                           couch_docs_disk_size = VBucketsDiskSize,
                           couch_docs_data_size = VBucketsDataSize,
                           couch_views_disk_size = ViewsDiskSize,
                           couch_views_data_size = ViewsDataSize} = CouchStats,
    [{couch_docs_actual_disk_size, DocsActualDiskSize},
     {couch_views_actual_disk_size, ViewsActualDiskSize},
     {couch_docs_disk_size, VBucketsDiskSize},
     {couch_docs_data_size, VBucketsDataSize},
     {couch_views_disk_size, ViewsDiskSize},
     {couch_views_data_size, ViewsDataSize}].

parse_couch_stats(_TS, CouchStats, undefined = _LastTS, _, _) ->
    Basic = build_basic_couch_stats(CouchStats),
    {lists:sort([{couch_views_ops, 0.0} | Basic]), []};
parse_couch_stats(TS, CouchStats, LastTS, LastViewsStats0, MinFileSize) ->
    BasicThings = build_basic_couch_stats(CouchStats),
    #ns_server_couch_stats{per_ddoc_stats = ViewStats} = CouchStats,
    LastViewsStats = case LastViewsStats0 of
                         undefined -> [];
                         _ -> LastViewsStats0
                     end,
    TSDelta = TS - LastTS,
    WithDiffedOps =
        case TSDelta > 0 of
            true ->
                diff_view_accesses_loop(TSDelta, LastViewsStats, ViewStats);
            false ->
                [{Sig, DiskS, DataS, 0} || {Sig, DiskS, DataS, _} <- ViewStats]
        end,
    AggregatedOps = lists:sum([Ops || {_, _, _, Ops} <- WithDiffedOps]),
    LL = [begin
              DiskKey = iolist_to_binary([<<"views/">>, Sig, <<"/disk_size">>]),
              DataKey = iolist_to_binary([<<"views/">>, Sig, <<"/data_size">>]),
              OpsKey = iolist_to_binary([<<"views/">>, Sig, <<"/accesses">>]),
              DataS = maybe_adjust_data_size(DataS0, DiskS, MinFileSize),

              [{DiskKey, DiskS},
               {DataKey, DataS},
               {OpsKey, OpsSec}]
          end || {Sig, DiskS, DataS0, OpsSec} <- WithDiffedOps],
    {lists:sort(lists:append([[{couch_views_ops, AggregatedOps}], BasicThings | LL])),
     ViewStats}.

%% Tests

basic_parse_couch_stats_test() ->
    CouchStatsRecord = #ns_server_couch_stats{couch_docs_actual_disk_size = 1,
                                              couch_views_actual_disk_size = 2,
                                              couch_docs_disk_size = 3,
                                              couch_docs_data_size = 4,
                                              couch_views_disk_size = 5,
                                              couch_views_data_size = 6,
                                              per_ddoc_stats = [{<<"a">>, 8, 9, 10},
                                                                {<<"b">>, 11, 12, 13}]},
    ExpectedOut1Pre = [{couch_docs_actual_disk_size, 1},
                       {couch_views_actual_disk_size, 2},
                       {couch_docs_disk_size, 3},
                       {couch_docs_data_size, 4},
                       {couch_views_disk_size, 5},
                       {couch_views_data_size, 6},
                       {couch_views_ops, 0.0}]
        ++ [{<<"views/a/disk_size">>, 8},
            {<<"views/a/data_size">>, 9},
            {<<"views/a/accesses">>, 0.0},
            {<<"views/b/disk_size">>, 11},
            {<<"views/b/data_size">>, 12},
            {<<"views/b/accesses">>, 0.0}],
    ExpectedOut1 = lists:sort([{K, V} || {K, V} <- ExpectedOut1Pre,
                                         not is_binary(K)]),
    ExpectedOut2 = lists:sort(ExpectedOut1Pre),
    {Out1, State1} = parse_couch_stats(1000, CouchStatsRecord, undefined, undefined, 0),
    ?debugFmt("Got first result~n~p~n~p", [Out1, State1]),
    {Out2, State2} = parse_couch_stats(2000, CouchStatsRecord, 1000, State1, 0),
    ?debugFmt("Got second result~n~p~n~p", [Out2, State2]),
    ?assertEqual(CouchStatsRecord#ns_server_couch_stats.per_ddoc_stats, State2),
    ?assertEqual(ExpectedOut1, Out1),
    ?assertEqual(ExpectedOut2, Out2).
