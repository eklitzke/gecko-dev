/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

const { RootActor } = require("devtools/server/actors/root");
const { DebuggerServer } = require("devtools/server/main");

/**
 * Root actor that doesn't have the bulk trait.
 */
function createRootActor(connection) {
  let root = new RootActor(connection, {
    globalActorFactories: DebuggerServer.globalActorFactories
  });
  root.applicationType = "xpcshell-tests";
  root.traits = {
    bulk: false
  };
  return root;
}

exports.register = function(handle) {
  handle.setRootActor(createRootActor);
};

exports.unregister = function(handle) {
  handle.setRootActor(null);
};
