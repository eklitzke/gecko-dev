#!/usr/bin/env python

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
from __future__ import absolute_import

import json
import os
import sys
import time

import mozinfo

from mozlog import commandline, get_default_logger
from mozprofile import create_profile
from mozrunner import runners

# need this so raptor imports work both from /raptor and via mach
here = os.path.abspath(os.path.dirname(__file__))
webext_dir = os.path.join(os.path.dirname(here), 'webext')
sys.path.insert(0, here)

try:
    from mozbuild.base import MozbuildObject
    build = MozbuildObject.from_environment(cwd=here)
except ImportError:
    build = None

from cmdline import parse_args
from control_server import RaptorControlServer
from gen_test_config import gen_test_config
from outputhandler import OutputHandler
from playback import get_playback
from manifest import get_raptor_test_list


class Raptor(object):
    """Container class for Raptor"""

    def __init__(self, app, binary):
        self.config = {}
        self.config['app'] = app
        self.config['binary'] = binary
        self.config['platform'] = mozinfo.os

        self.raptor_venv = os.path.join(os.getcwd(), 'raptor-venv')
        self.log = get_default_logger(component='raptor')
        self.control_server = None
        self.playback = None

        # Create the profile
        self.profile = create_profile(self.config['app'])

        # Merge in base profiles
        with open(os.path.join(self.profile_data_dir, 'profiles.json'), 'r') as fh:
            base_profiles = json.load(fh)['raptor']

        for name in base_profiles:
            path = os.path.join(self.profile_data_dir, name)
            self.log.info("Merging profile: {}".format(path))
            self.profile.merge(path)

        # Create the runner
        self.output_handler = OutputHandler()
        process_args = {
            'processOutputLine': [self.output_handler],
        }
        runner_cls = runners[app]
        self.runner = runner_cls(
            binary, profile=self.profile, process_args=process_args)

    @property
    def profile_data_dir(self):
        if 'MOZ_DEVELOPER_REPO_DIR' in os.environ:
            return os.path.join(os.environ['MOZ_DEVELOPER_REPO_DIR'], 'testing', 'profiles')
        if build:
            return os.path.join(build.topsrcdir, 'testing', 'profiles')
        return os.path.join(here, 'profile_data')

    def start_control_server(self):
        self.control_server = RaptorControlServer()
        self.control_server.start()

    def get_playback_config(self, test):
        self.config['playback_tool'] = test.get('playback')
        self.log.info("test uses playback tool: %s " % self.config['playback_tool'])
        self.config['playback_binary_manifest'] = test.get('playback_binary_manifest', None)
        _key = 'playback_binary_zip_%s' % self.config['platform']
        self.config['playback_binary_zip'] = test.get(_key, None)
        self.config['playback_pageset_manifest'] = test.get('playback_pageset_manifest', None)
        _key = 'playback_pageset_zip_%s' % self.config['platform']
        self.config['playback_pageset_zip'] = test.get(_key, None)
        self.config['playback_recordings'] = test.get('playback_recordings', None)

    def run_test(self, test, timeout=None):
        self.log.info("starting raptor test: %s" % test['name'])
        gen_test_config(self.config['app'], test['name'], self.control_server.port)

        self.profile.addons.install(os.path.join(webext_dir, 'raptor'))

        # some tests require tools to playback the test pages
        if test.get('playback', None) is not None:
            self.get_playback_config(test)
            # startup the playback tool
            self.playback = get_playback(self.config)

        self.runner.start()

        first_time = int(time.time()) * 1000
        proc = self.runner.process_handler
        self.output_handler.proc = proc

        try:
            self.runner.wait(timeout)
        finally:
            try:
                self.runner.check_for_crashes()
            except NotImplementedError:  # not implemented for Chrome
                pass

        if self.playback is not None:
            self.playback.stop()

        if self.runner.is_running():
            self.log("Application timed out after {} seconds".format(timeout))
            self.runner.stop()

        proc.output.append(
            "__startBeforeLaunchTimestamp%d__endBeforeLaunchTimestamp"
            % first_time)
        proc.output.append(
            "__startAfterTerminationTimestamp%d__endAfterTerminationTimestamp"
            % (int(time.time()) * 1000))

    def process_results(self):
        self.log.info('todo: process results and dump in PERFHERDER_JSON blob')
        self.log.info('- or - do we want the control server to do that?')

    def clean_up(self):
        self.control_server.stop()
        self.runner.stop()
        self.log.info("raptor finished")


def main(args=sys.argv[1:]):
    args = parse_args()
    commandline.setup_logging('raptor', args, {'tbpl': sys.stdout})
    LOG = get_default_logger(component='raptor-main')

    # if a test name specified on command line, and it exists, just run that one
    # otherwise run all available raptor tests that are found for this browser
    raptor_test_list = get_raptor_test_list(args)

    # ensure we have at least one valid test to run
    if len(raptor_test_list) == 0:
        LOG.critical("abort: no tests found")
        sys.exit(1)

    LOG.info("raptor tests scheduled to run:")
    for next_test in raptor_test_list:
        LOG.info(next_test['name'])

    raptor = Raptor(args.app, args.binary)

    raptor.start_control_server()

    for next_test in raptor_test_list:
        raptor.run_test(next_test)

    raptor.process_results()
    raptor.clean_up()


if __name__ == "__main__":
    main()
