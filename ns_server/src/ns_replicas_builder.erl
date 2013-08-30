%% @author Couchbase, Inc <info@couchbase.com>
%% @copyright 2012 Couchbase, Inc.
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
%% @doc this implements creating replication stream for particular
%% vbucket from some node to bunch of other nodes and waiting until
%% all backfills are done. It is also possible to shutdown some
%% replications earlier then others.
%%
%% NOTE: this code can be structured as gen_server, but I decided
%% against that. Reason is that I'd like to keep logic straight and
%% fsm-less, but if you, my dear reader, is planning any extra
%% features here, please, consider refactoring it into gen_server.

-module(ns_replicas_builder).

-include("ns_common.hrl").

-export([spawn_link/6]).

%% @doc spawns replicas builder for given bucket, vbucket, source and
%% destination node(s). AfterDone will be called after all backfills
%% are done, so it can notify whoever is interested in this event. For
%% JustBackfillNodes it'll backfill replicas just as for any other
%% destination node, but it'll stop replication when backfill is done
%% (and before AfterDone is called). Replicas on ReplicateIntoNodes
%% will be built and replication will continue maintaining them even
%% when AfterDone is called and after it returns.
%%
%% When replicas are built and AfterDone is called this process will
%% wait EXIT signal from parent, on which it'll reliably terminate
%% child replicator and exit.
-spec spawn_link(Bucket::bucket_name(), VBucket::vbucket_id(), SrcNode::node(),
                 ReplicateIntoNodes::[node()], JustBackfillNodes::[node()],
                 AfterDone::fun(() -> any()))-> pid().
spawn_link(Bucket, VBucket, SrcNode, ReplicateIntoNodes, JustBackfillNodes, AfterDone) ->
    proc_lib:spawn_link(erlang, apply,
                        [fun build_replicas_main/6,
                         [Bucket, VBucket, SrcNode, ReplicateIntoNodes, JustBackfillNodes, AfterDone]]).

%% implements poor man's exponential back off
sleeps_so_far_to_sleep_interval(SleepsSoFar) ->
    if
        SleepsSoFar < 5 ->
            10;
        SleepsSoFar < 10 ->
            30;
        true ->
            100
    end.

-spec build_replicas_main(bucket_name(), vbucket_id(), node(), [node()], [node()], fun(() -> ok)) -> ok.
build_replicas_main(Bucket, VBucket, SrcNode, ReplicateIntoNodes, JustBackfillNodes, AfterDone) ->
    erlang:process_flag(trap_exit, true),
    case (JustBackfillNodes -- ReplicateIntoNodes) =:= JustBackfillNodes andalso
                     (ReplicateIntoNodes -- JustBackfillNodes) =:= ReplicateIntoNodes of
        false ->
            exit({badarg_on_nodes, ReplicateIntoNodes, JustBackfillNodes});
        _ -> ok
    end,

    StopEarlyReplicators = [ns_replicas_builder_utils:spawn_replica_builder(Bucket, VBucket, SrcNode, DNode)
                            || DNode <- JustBackfillNodes],
    ContinuousReplicators = [ns_replicas_builder_utils:spawn_replica_builder(Bucket, VBucket, SrcNode, DNode)
                             || DNode <- ReplicateIntoNodes],
    Replicators = StopEarlyReplicators ++ ContinuousReplicators,

    misc:try_with_maybe_ignorant_after(
      fun () ->
              observe_wait_all_done(Bucket, VBucket, SrcNode, JustBackfillNodes ++ ReplicateIntoNodes,
                                    fun (SleepsSoFar) ->
                                            SleepMS = sleeps_so_far_to_sleep_interval(SleepsSoFar),
                                            system_stats_collector:increment_counter(replica_builder_sleeps, 1),
                                            system_stats_collector:increment_counter(replica_builder_sleep_amount, SleepMS),
                                            receive
                                                {'EXIT', From, Reason} = ExitMsg ->
                                                    case lists:member(From, Replicators) of
                                                        true ->
                                                            ?log_error("Got premature exit from one of ebucketmigrators: ~p", [ExitMsg]),
                                                            self() ! ExitMsg, % we'll process it again in after block
                                                            exit({replicator_died, ExitMsg});
                                                        _ ->
                                                            ?log_info("Got exit not from child ebucketmigrator. Assuming it's our parent: ~p", [ExitMsg]),
                                                            exit(Reason)
                                                    end
                                            after SleepMS ->
                                                    ok
                                            end
                                    end),

              misc:sync_shutdown_many_i_am_trapping_exits(StopEarlyReplicators),

              AfterDone(),

              %% when replications are up to date, just wait death
              %% signal
              receive
                  {'EXIT', _, _} = ExMsg ->
                      self() ! ExMsg
              end
      end,
      fun () ->
              misc:try_with_maybe_ignorant_after(
                fun () ->
                        misc:sync_shutdown_many_i_am_trapping_exits(ContinuousReplicators)
                end,
                fun () ->
                        ns_replicas_builder_utils:kill_tap_names(Bucket, VBucket, SrcNode, JustBackfillNodes ++ ReplicateIntoNodes)
                end)
      end),
    receive
        {'EXIT', _From, Reason} = ExitMsg ->
            ?log_info("Got exit: ~p", [ExitMsg]),
            exit(Reason)
    after 0 ->
            ok
    end.



observe_wait_all_done(Bucket, VBucket, SrcNode, DstNodes, Sleeper) ->
    TapNames = sets:from_list(
                 [iolist_to_binary(ns_replicas_builder_utils:tap_name(VBucket, SrcNode, DN))
                  || DN <- DstNodes]),
    observe_wait_all_done_old_style_loop(Bucket, SrcNode, Sleeper, TapNames, 0),
    wait_checkpoint_opened(Bucket, VBucket, DstNodes, Sleeper, 0).

observe_wait_all_done_old_style_loop(Bucket, SrcNode, Sleeper, TapNames, SleepsSoFar) ->
    case sets:size(TapNames) of
        0 ->
            ok;
        _ ->
            if SleepsSoFar > 0 ->
                    system_stats_collector:increment_counter(replicas_builder_backfill_sleeps, 1),
                    Sleeper(SleepsSoFar);
               true ->
                    ok
            end,
            {ok, PList} = ns_memcached:stats(SrcNode, Bucket, <<"tap">>),
            DoneTaps = extract_complete_taps(PList, TapNames),
            NewTapNames = sets:subtract(TapNames, DoneTaps),
            observe_wait_all_done_old_style_loop(Bucket, SrcNode, Sleeper, NewTapNames, SleepsSoFar+1)
    end.

-spec filter_true_producers(list(), set(), binary()) -> [binary()].
filter_true_producers(PList, TapNamesSet, StatName) ->
    [TapName
     || {<<"eq_tapq:replication_", Key/binary>>, <<"true">>} <- PList,
        TapName <- case misc:split_binary_at_char(Key, $:) of
                       {TapName0, StatName} ->
                           sets:is_element(TapName0, TapNamesSet),
                           [TapName0];
                       _ ->
                           []
                   end].

extract_complete_taps(PList, TapNames) ->
    sets:from_list(filter_true_producers(PList, TapNames, <<"backfill_completed">>)).

%% this makes sure all nodes have open checkpoint and that they all
%% have same open checkpoint
wait_checkpoint_opened(_Bucket, _VBucket, [], _Sleeper, _SleepsSoFar) ->
    ok;
wait_checkpoint_opened(Bucket, VBucket, DstNodes, Sleeper, SleepsSoFar) ->
    case SleepsSoFar =:= 0 of
        false ->
            system_stats_collector:increment_counter(replicas_builder_checkpoint_sleeps, 1),
            ?log_debug("Incremented replicas_builder_checkpoint_sleeps"),
            Sleeper(SleepsSoFar);
        true -> ok
    end,
    Checkpoints = ns_memcached:get_vbucket_open_checkpoint(DstNodes, Bucket, VBucket),
    [case Checkpoint of
         missing ->
             ?log_error("Node ~p did not have checkpoint stat for vbucket: ~p", [N, VBucket]),
             exit({missing_checkpoint_stat, N, VBucket});
         _ ->
             ok
     end || {N, Checkpoint} <- Checkpoints],
    [{_, FirstCheckpoint} | _] = Checkpoints,
    Done = case FirstCheckpoint of
               0 ->
                   false;
               _ ->
                   NodesLeft = [N || {N, Checkpoint} <- Checkpoints,
                                     Checkpoint =/= FirstCheckpoint],
                   NodesLeft =:= []
           end,
    case Done of
        true ->
            ok;
        false ->
            wait_checkpoint_opened(Bucket, VBucket, DstNodes, Sleeper, SleepsSoFar+1)
    end.
