/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

watchman_client_subscription::watchman_client_subscription(
    const std::shared_ptr<w_root_t>& root,
    std::weak_ptr<watchman_client> client)
    : root(root), weakClient(client) {}

std::shared_ptr<watchman_user_client>
watchman_client_subscription::lockClient() {
  auto client = weakClient.lock();
  if (client) {
    return std::dynamic_pointer_cast<watchman_user_client>(client);
  }
  return nullptr;
}

watchman_client_subscription::~watchman_client_subscription() {
  auto client = lockClient();
  if (client) {
    client->unsubByName(name);
  }
}

bool watchman_user_client::unsubByName(const w_string& name) {
  auto subIter = subscriptions.find(name);
  if (subIter == subscriptions.end()) {
    return false;
  }

  unilateralSub.erase(subIter->second);
  subscriptions.erase(subIter);

  return true;
}

static void w_run_subscription_rules(
    struct watchman_user_client* client,
    struct watchman_client_subscription* sub,
    const std::shared_ptr<w_root_t>& root);

void watchman_client_subscription::processSubscription() {
  auto client = lockClient();
  if (!client) {
    watchman::log(
        watchman::ERR,
        "encountered a vacated client while running subscription rules\n");
    return;
  }

  bool defer = false;
  bool drop = false;
  w_string policy_name;

  auto position = root->inner.view->getMostRecentRootNumberAndTickValue();

  watchman::log(
      watchman::DBG,
      "sub=",
      this,
      " ",
      name,
      ", last=",
      last_sub_tick,
      " pending=",
      position.ticks,
      "\n");

  if (last_sub_tick != position.ticks) {
    auto asserted_states = root->asserted_states.rlock();
    if (!asserted_states->empty() && !drop_or_defer.empty()) {
      // There are 1 or more states asserted and this subscription
      // has some policy for states.  Figure out what we should do.
      for (auto& policy_iter : drop_or_defer) {
        auto name = policy_iter.first;
        bool policy_is_drop = policy_iter.second;

        if (asserted_states->find(name) == asserted_states->end()) {
          continue;
        }

        if (!defer) {
          // This policy is active
          defer = true;
          policy_name = name;
        }

        if (policy_is_drop) {
          drop = true;

          // If we're dropping, we don't need to look at any
          // other policies
          policy_name = name;
          break;
        }
        // Otherwise keep looking until we find a drop
      }
    }

    bool executeQuery = true;

    if (drop) {
      // fast-forward over any notifications while in the drop state
      last_sub_tick = position.ticks;
      query->since_spec = watchman::make_unique<w_clockspec>(position);
      watchman::log(
          watchman::DBG,
          "dropping subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated. Advanced ticks to ",
          last_sub_tick,
          "\n");
      executeQuery = false;
    }

    if (defer) {
      watchman::log(
          watchman::DBG,
          "deferring subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated\n");
      executeQuery = false;
    }

    if (vcs_defer && root->inner.view->isVCSOperationInProgress()) {
      watchman::log(
          watchman::DBG,
          "deferring subscription notifications for ",
          name,
          " until VCS operations complete\n");
      executeQuery = false;
    }

    if (executeQuery) {
      w_run_subscription_rules(client.get(), this, root);
      last_sub_tick = position.ticks;
    }
  } else {
    watchman::log(watchman::DBG, "subscription ", name, " is up to date\n");
  }
}

static void update_subscription_ticks(struct watchman_client_subscription *sub,
    w_query_res *res) {
  // create a new spec that will be used the next time
  sub->query->since_spec =
      watchman::make_unique<w_clockspec>(res->clockAtStartOfQuery);
}

static json_ref build_subscription_results(
    struct watchman_client_subscription* sub,
    const std::shared_ptr<w_root_t>& root,
    ClockPosition& position) {
  w_query_res res;
  auto since_spec = sub->query->since_spec.get();

  if (since_spec && since_spec->tag == w_cs_clock) {
    w_log(
        W_LOG_DBG,
        "running subscription %s rules since %" PRIu32 "\n",
        sub->name.c_str(),
        since_spec->clock.position.ticks);
  } else {
    w_log(
        W_LOG_DBG,
        "running subscription %s rules (no since)\n",
        sub->name.c_str());
  }

  // Subscriptions never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  sub->query->sync_timeout = std::chrono::milliseconds(0);
  // We're called by the io thread, so there's little chance that the root
  // could be legitimately blocked by something else.  That means that we
  // can use a short lock_timeout
  sub->query->lock_timeout =
      uint32_t(root->config.getInt("subscription_lock_timeout_ms", 100));
  w_log(W_LOG_DBG, "running subscription %s %p\n", sub->name.c_str(), sub);

  if (!w_query_execute_locked(sub->query.get(), root, &res, time_generator)) {
    w_log(
        W_LOG_ERR,
        "error running subscription %s query: %s",
        sub->name.c_str(),
        res.errmsg);
    return nullptr;
  }

  w_log(
      W_LOG_DBG,
      "subscription %s generated %" PRIu32 " results\n",
      sub->name.c_str(),
      uint32_t(res.resultsArray.array().size()));

  position = res.clockAtStartOfQuery;

  if (res.resultsArray.array().empty()) {
    update_subscription_ticks(sub, &res);
    return nullptr;
  }

  auto response = make_response();

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock) {
    response.set(
        "since", w_string_to_json(since_spec->clock.position.toClockString()));
  }
  update_subscription_ticks(sub, &res);

  response.set(
      {{"is_fresh_instance", json_boolean(res.is_fresh_instance)},
       {"clock", w_string_to_json(res.clockAtStartOfQuery.toClockString())},
       {"files", std::move(res.resultsArray)},
       {"root", w_string_to_json(root->root_path)},
       {"subscription", w_string_to_json(sub->name)},
       {"unilateral", json_true()}});

  return response;
}

static void w_run_subscription_rules(
    struct watchman_user_client* client,
    struct watchman_client_subscription* sub,
    const std::shared_ptr<w_root_t>& root) {
  ClockPosition position;

  auto response = build_subscription_results(sub, root, position);

  if (!response) {
    return;
  }

  add_root_warnings_to_response(response, root);

  client->enqueueResponse(std::move(response), false);
}

/* unsubscribe /root subname
 * Cancels a subscription */
static void cmd_unsubscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  const char *name;
  bool deleted{false};
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto jstr = json_array_get(args, 2);
  name = json_string_value(jstr);
  if (!name) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    return;
  }

  auto sname = json_to_w_string(jstr);
  deleted = client->unsubByName(sname);

  auto resp = make_response();
  resp.set({{"unsubscribe", typed_string_to_json(name)},
            {"deleted", json_boolean(deleted)}});

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("unsubscribe", cmd_unsubscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
static void cmd_subscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  std::shared_ptr<watchman_client_subscription> sub;
  json_ref resp, initial_subscription_results;
  json_ref jfield_list;
  json_ref jname;
  std::shared_ptr<w_query> query;
  json_ref query_spec;
  char *errmsg;
  int defer = true; /* can't use bool because json_unpack requires int */
  json_ref defer_list;
  json_ref drop_list;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;

  if (json_array_size(args) != 4) {
    send_error_response(client, "wrong number of arguments for subscribe");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  jname = args.at(2);
  if (!json_is_string(jname)) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    return;
  }

  query_spec = args.at(3);

  query = w_query_parse(root, query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    return;
  }

  defer_list = query_spec.get_default("defer");
  if (defer_list && !json_is_array(defer_list)) {
    send_error_response(client, "defer field must be an array of strings");
    return;
  }

  drop_list = query_spec.get_default("drop");
  if (drop_list && !json_is_array(drop_list)) {
    send_error_response(client, "drop field must be an array of strings");
    return;
  }

  sub = std::make_shared<watchman_client_subscription>(
      root, client->shared_from_this());

  sub->name = json_to_w_string(jname);
  sub->query = query;

  json_unpack(query_spec, "{s?:b}", "defer_vcs", &defer);
  sub->vcs_defer = defer;

  if (drop_list || defer_list) {
    size_t i;

    if (defer_list) {
      for (i = 0; i < json_array_size(defer_list); i++) {
        sub->drop_or_defer[json_to_w_string(json_array_get(defer_list, i))] =
            false;
      }
    }
    if (drop_list) {
      for (i = 0; i < json_array_size(drop_list); i++) {
        sub->drop_or_defer[json_to_w_string(json_array_get(drop_list, i))] =
            true;
      }
    }
  }

  // Connect the root to our subscription
  {
    auto clientRef = client->shared_from_this();
    client->unilateralSub.insert(std::make_pair(
        sub, root->unilateralResponses->subscribe([clientRef, sub]() {
          w_event_set(clientRef->ping);
        })));
  }

  client->subscriptions[sub->name] = sub;

  resp = make_response();
  resp.set("subscribe", json_ref(jname));

  add_root_warnings_to_response(resp, root);
  ClockPosition position;
  initial_subscription_results =
      build_subscription_results(sub.get(), root, position);
  resp.set("clock", w_string_to_json(position.toClockString()));

  send_and_dispose_response(client, std::move(resp));
  if (initial_subscription_results) {
    send_and_dispose_response(client, std::move(initial_subscription_results));
  }
}
W_CMD_REG("subscribe", cmd_subscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
