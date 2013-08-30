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
%% Monitor and maintain the vbucket layout of each bucket.
%%
-module(ns_janitor).

-include("ns_common.hrl").

-include_lib("eunit/include/eunit.hrl").

-export([cleanup/2, stop_rebalance_status/1]).


-spec cleanup(Bucket::bucket_name(), Options::list()) -> ok | {error, wait_for_memcached_failed, [node()]}.
cleanup(Bucket, Options) ->
    FullConfig = ns_config:get(),
    case ns_bucket:get_bucket(Bucket, FullConfig) of
        not_present ->
            ok;
        {ok, BucketConfig} ->
            case ns_bucket:bucket_type(BucketConfig) of
                membase ->
                    cleanup_with_membase_bucket_check_servers(Bucket, Options, BucketConfig, FullConfig);
                _ -> ok
            end
    end.

cleanup_with_membase_bucket_check_servers(Bucket, Options, BucketConfig, FullConfig) ->
    case compute_servers_list_cleanup(BucketConfig, FullConfig) of
        none ->
            cleanup_with_membase_bucket_check_map(Bucket, Options, BucketConfig);
        {update_servers, NewServers} ->
            ?log_debug("janitor decided to update servers list"),
            ns_bucket:set_servers(Bucket, NewServers),
            cleanup(Bucket, Options)
    end.

cleanup_with_membase_bucket_check_map(Bucket, Options, BucketConfig) ->
    case proplists:get_value(map, BucketConfig, []) of
        [] ->
            Servers = proplists:get_value(servers, BucketConfig, []),
            true = (Servers =/= []),
            case janitor_agent:wait_for_bucket_creation(Bucket, Servers) of
                [_|_] = Down ->
                    ?log_info("~s: Some nodes (~p) are still not ready to see if we need to recover past vbucket map", [Bucket, Down]),
                    {error, wait_for_memcached_failed, Down};
                [] ->
                    NumVBuckets = proplists:get_value(num_vbuckets, BucketConfig),
                    NumReplicas = ns_bucket:num_replicas(BucketConfig),
                    NewMap = case ns_janitor_map_recoverer:read_existing_map(Bucket, Servers, NumVBuckets, NumReplicas) of
                                 {ok, M} ->
                                     M;
                                 {error, no_map} ->
                                     ?log_info("janitor decided to generate initial vbucket map"),
                                     ns_rebalancer:generate_initial_map(BucketConfig)
                             end,

                    case ns_rebalancer:unbalanced(NewMap, Servers) of
                        false ->
                            ns_bucket:update_vbucket_map_history(NewMap, ns_bucket:config_to_map_options(BucketConfig));
                        true ->
                            ok
                    end,

                    ns_bucket:set_map(Bucket, NewMap),
                    cleanup(Bucket, Options)
            end;
        _ ->
            cleanup_with_membase_bucket_vbucket_map(Bucket, Options, BucketConfig)
    end.

cleanup_with_membase_bucket_vbucket_map(Bucket, Options, BucketConfig) ->
    Servers = proplists:get_value(servers, BucketConfig, []),
    true = (Servers =/= []),
    {ok, States, Zombies} = janitor_agent:query_states(Bucket, Servers, proplists:get_value(timeout, Options)),
    cleanup_with_states(Bucket, Options, BucketConfig, Servers, States, Zombies).

cleanup_with_states(Bucket, _Options, _BucketConfig, _Servers, _States, Zombies) when Zombies =/= [] ->
    ?log_error("Bucket ~p not yet ready on ~p", [Bucket, Zombies]),
    {error, wait_for_memcached_failed, Zombies};
cleanup_with_states(Bucket, Options, BucketConfig, Servers, States, [] = Zombies) ->
    {NewBucketConfig, IgnoredVBuckets} = compute_vbucket_map_fixup(Bucket, BucketConfig, States, [] = Zombies),

    case NewBucketConfig =:= BucketConfig of
        true ->
            ok;
        false ->
            ok = ns_bucket:set_bucket_config(Bucket, NewBucketConfig)
    end,

    ok = janitor_agent:apply_new_bucket_config(Bucket, Servers, Zombies, NewBucketConfig, IgnoredVBuckets, States),

    case Zombies =:= [] andalso proplists:get_bool(consider_stopping_rebalance_status, Options) of
        true ->
            maybe_stop_rebalance_status();
        _ -> ok
    end,

    janitor_agent:mark_bucket_warmed(Bucket, Servers),
    ok.

stop_rebalance_status(Fn) ->
    Sentinel = make_ref(),
    Fun = fun ({rebalance_status, Value}) ->
                  NewValue =
                      case Value of
                          running ->
                              Fn();
                          _ ->
                              Value
                      end,
                  {rebalance_status, NewValue};
              ({rebalancer_pid, _}) ->
                  {rebalancer_pid, undefined};
              (Other) ->
                  Other
          end,

    ok = ns_config:update(Fun, Sentinel).

maybe_stop_rebalance_status() ->
    Status = try ns_orchestrator:rebalance_progress_full()
             catch E:T ->
                     ?log_error("cannot reach orchestrator: ~p:~p", [E,T]),
                     error
             end,
    case Status of
        %% if rebalance is not actually running according to our
        %% orchestrator, we'll consider checking config and seeing if
        %% we should unmark is at not running
        not_running ->
            stop_rebalance_status(
              fun () ->
                      ale:info(?USER_LOGGER,
                               "Resetting rebalance status "
                               "since it's not really running"),
                      {none, <<"Rebalance stopped by janitor.">>}
              end);
        _ ->
            ok
    end.

%% !!! only purely functional code below (with notable exception of logging) !!!
%% lets try to keep as much as possible logic below this line

compute_servers_list_cleanup(BucketConfig, FullConfig) ->
    case proplists:get_value(servers, BucketConfig) of
        [] ->
            NewServers = ns_cluster_membership:active_nodes(FullConfig),
            {update_servers, NewServers};
        Servers when is_list(Servers) ->
            none;
        Else ->
            ?log_error("Some garbage in servers field: ~p", [Else]),
            BucketConfig1 = [{servers, []} | lists:keydelete(servers, 1, BucketConfig)],
            compute_servers_list_cleanup(BucketConfig1, FullConfig)
    end.

compute_vbucket_map_fixup(Bucket, BucketConfig, States, [] = Zombies) ->
    Map = proplists:get_value(map, BucketConfig, []),
    true = ([] =/= Map),
    FFMap = case proplists:get_value(fastForwardMap, BucketConfig) of
                undefined -> [];
                FFMap0 ->
                    case FFMap0 =:= [] orelse length(FFMap0) =:= length(Map) of
                        true ->
                            FFMap0;
                        false ->
                            ?log_warning("fast forward map length doesn't match map length. Ignoring it"),
                            []
                    end
            end,
    EffectiveFFMap = case FFMap of
                         [] ->
                             [[] || _ <- Map];
                         _ ->
                             FFMap
                     end,
    MapLen = length(Map),
    EnumeratedChains = lists:zip3(lists:seq(0, MapLen - 1),
                                  Map,
                                  EffectiveFFMap),
    MapUpdates = [sanify_chain(Bucket, States, Chain, FutureChain, VBucket, Zombies)
                  || {VBucket, Chain, FutureChain} <- EnumeratedChains],
    IgnoredVBuckets = [VBucket || {VBucket, ignore} <- lists:zip(lists:seq(0, MapLen - 1), MapUpdates)],
    NewMap = [case NewChain of
                  ignore -> OldChain;
                  _ -> NewChain
              end || {NewChain, OldChain} <- lists:zip(MapUpdates, Map)],
    NewBucketConfig = case NewMap =:= Map of
                          true ->
                              BucketConfig;
                          false ->
                              ?log_debug("Janitor decided to update vbucket map"),
                              lists:keyreplace(map, 1, BucketConfig, {map, NewMap})
                      end,
    {NewBucketConfig, IgnoredVBuckets}.

sanify_chain(Bucket, States, Chain, FutureChain, VBucket, Zombies) ->
    NewChain = do_sanify_chain(Bucket, States, Chain, FutureChain, VBucket, Zombies),
    %% Fill in any missing replicas
    case is_list(NewChain) andalso length(NewChain) < length(Chain) of
        false ->
            NewChain;
        true ->
            NewChain ++ lists:duplicate(length(Chain) - length(NewChain),
                                        undefined)
    end.

%% this will decide what vbucket map chain is right for this vbucket
do_sanify_chain(Bucket, States, Chain, FutureChain, VBucket, [] = Zombies) ->
    NodeStates = [{N, S} || {N, V, S} <- States, V == VBucket],
    ChainStates = lists:map(fun (N) ->
                                    case lists:keyfind(N, 1, NodeStates) of
                                        %% NOTE: cannot use code below
                                        %% due to "stupid" warning by
                                        %% dialyzer. Yes indeed our
                                        %% Zombies is always [] as of
                                        %% now
                                        %%
                                        %% false -> {N, case lists:member(N, Zombies) of
                                        %%                  true -> zombie;
                                        %%                  _ -> missing
                                        %%              end};
                                        false ->
                                            [] = Zombies,
                                            missing;
                                        X -> X
                                    end
                            end, Chain),
    ExtraStates = [X || X = {N, _} <- NodeStates,
                        not lists:member(N, Chain)],
    case ChainStates of
        [{undefined, _}|_] ->
            %% if for some reason (failovers most likely) we don't
            %% have master, there's not much we can do. Or at least
            %% that's how we have been behaving since early days.
            Chain;
        [{Master, State}|ReplicaStates] when State /= active andalso State /= zombie ->
            %% we know master according to map is not active. See if
            %% there's anybody else who is active and if we need to update map
            case [N || {N, active} <- ReplicaStates ++ ExtraStates] of
                [] ->
                    %% We'll let the next pass catch the replicas.
                    ?log_info("Setting vbucket ~p in ~p on ~p from ~p to active.",
                              [VBucket, Bucket, Master, State]),
                    %% nobody else (well, assuming zombies are not
                    %% active) is active. Let's activate according to vbucket map
                    Chain;
                [Node] ->
                    %% somebody else is active.
                    PickFutureChain =
                        case FutureChain of
                            [Node | _] ->
                                %% if active is future master check rest of future chain
                                [FFMasterState | FFReplicaStates] = [proplists:get_value(N, NodeStates)
                                                                     || N <- FutureChain,
                                                                        N =/= undefined],
                                %% and if everything fits -- cool
                                FFMasterState =:= active
                                    andalso lists:all(fun (replica) -> true;
                                                          (_) -> false
                                                      end, FFReplicaStates);
                            _ ->
                                false
                        end,
                    case PickFutureChain of
                        true ->
                            ?log_warning("Master for vbucket ~p in ~p is not active, but entire fast-forward map chain fits (~p), so using it.", [VBucket, Bucket, FutureChain]),
                            FutureChain;
                        false ->
                            %% One active node, but it's not the
                            %% master.
                            %%
                            %% It's not fast-forward map master, so
                            %% we'll just update vbucket map. Note
                            %% behavior below with losing replicas
                            %% makes little sense as of
                            %% now. Especially with star
                            %% replication. But we can adjust it
                            %% later.
                            case misc:position(Node, Chain) of
                                false ->
                                    %% It's an extra node
                                    ?log_warning(
                                       "Master for vbucket ~p in ~p is not active, but ~p is, so making that the master.",
                                       [VBucket, Bucket, Node]),
                                    [Node];
                                Pos ->
                                    [Node|lists:nthtail(Pos, Chain)]
                            end
                    end;
                Nodes ->
                    ?log_error("Extra active nodes ~p for vbucket ~p in ~p. This should never happen!",
                               [Nodes, Bucket, VBucket]),
                    %% just do nothing if there are two active
                    %% vbuckets and both are not master according to
                    %% vbucket map
                    ignore
            end;
        _ ->
            %% NOTE: here we know that master is either active or zombie
            %% just keep existing vbucket map chain
            Chain
    end.
