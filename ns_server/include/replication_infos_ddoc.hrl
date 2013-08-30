-define(REPLICATION_INFOS_MAP, <<
"function (d, meta) {\n"
"  d._id = meta.id;\n"
"  if (d.type === 'xdc') {\n"
"    emit([d._id], d);\n"
"  } else if (d.node && d.replication_doc_id && d.replication_fields) {\n"
"    d.replication_fields._id = d.replication_doc_id;\n"
"    emit([d.replication_doc_id, d._id], d);\n"
"  }\n"
"}\n"
>>).

-define(REPLICATION_INFOS_REDUCE, <<
"function (keys, values, rereduce) {\n"
"  var result_state = null;\n"
"  var state_ts;\n"
"  var have_replicator_doc = false;\n"
"  var count = 0;\n"
"  var replication_fields = null;\n"
"\n"
"  function minTS(state_ts, ts) {\n"
"    return (state_ts == null) ? ts : (ts == null) ? state_ts : (ts < state_ts) ? ts : state_ts;\n"
"  }\n"
"\n"
"  function setState(state, ts) {\n"
"    if (result_state === state) {\n"
"      state_ts = minTS(state_ts, ts);\n"
"    } else {\n"
"      result_state = state;\n"
"      state_ts = ts;\n"
"    }\n"
"  }\n"
"\n"
"  function addReplicationFields(a, b) {\n"
"    if (a === undefined || b === undefined) {\n"
"      return;\n"
"    }\n"
"    var rv = {\n"
"      _id: a._id,\n"
"      source: a.source,\n"
"      target: a.target,\n"
"      continuous: a.continuous\n"
"    }\n"
"    if (b === null) {\n"
"      return rv;\n"
"    }\n"
"    if (rv._id !== b._id\n"
"        || rv.source !== b.source\n"
"        || rv.target !== b.target\n"
"        || rv.continuous !== b.continuous) {\n"
"      return;\n"
"    }\n"
"    return rv;\n"
"  }\n"
"\n"
"  values.forEach(function (d) {\n"
"    if (d.type === 'xdc') {\n"
"      have_replicator_doc = true;\n"
"      replication_fields = addReplicationFields(d, replication_fields);\n"
"      return;\n"
"    }\n"
"    replication_fields = addReplicationFields(d.replication_fields, replication_fields);\n"
"    have_replicator_doc = d.have_replicator_doc || have_replicator_doc;\n"
"    count += d.count ? d.count : 1;\n"
"    var state = d._replication_state;\n"
"    var ts = d._replication_state_time;\n"
"    if (state === undefined) {\n"
"      setState(state, ts);\n"
"    } else if (result_state === undefined) {\n"
"    } else if (state === 'triggered') {\n"
"      setState(state, ts);\n"
"    } else if (result_state === 'triggered') {\n"
"    } else if (state === 'cancelled') {\n"
"      setState(state, ts);\n"
"    } else if (result_state === 'cancelled') {\n"
"    } else if (state === 'error') {\n"
"      setState(state, ts);\n"
"    } else if (result_state === 'error') {\n"
"    } else if (state === 'completed') {\n"
"      setState(state, ts);\n"
"    }\n"
"  });\n"
"\n"
"  // NOTE: null signals lack of rows here, undefined means any row had\n"
"  // undefined\n"
"  if (result_state === null) {\n"
"    result_state = undefined;\n"
"  }\n"
"\n"
"  // NOTE: null signals lack of rows here, undefined means conflicting\n"
"  // fields\n"
"  if (replication_fields === null) {\n"
"    replication_fields = undefined;\n"
"  }\n"
"\n"
"  return {_replication_state: result_state,\n"
"          _replication_state_time: state_ts,\n"
"          replication_fields: replication_fields,\n"
"          have_replicator_doc: have_replicator_doc,\n"
"          count: count};\n"
"}\n"
>>).
