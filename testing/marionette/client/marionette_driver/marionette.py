# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import

import base64
import datetime
import json
import os
import socket
import sys
import time
import traceback
import warnings

from contextlib import contextmanager

from six import reraise

from . import errors
from . import transport
from .decorators import do_process_check
from .geckoinstance import GeckoInstance
from .keys import Keys
from .timeout import Timeouts

WEBELEMENT_KEY = "ELEMENT"
W3C_WEBELEMENT_KEY = "element-6066-11e4-a52e-4f735466cecf"


class HTMLElement(object):
    """Represents a DOM Element."""

    def __init__(self, marionette, id):
        self.marionette = marionette
        assert(id is not None)
        self.id = id

    def __str__(self):
        return self.id

    def __eq__(self, other_element):
        return self.id == other_element.id

    def find_element(self, method, target):
        """Returns an ``HTMLElement`` instance that matches the specified
        method and target, relative to the current element.

        For more details on this function, see the
        :func:`~marionette_driver.marionette.Marionette.find_element` method
        in the Marionette class.
        """
        return self.marionette.find_element(method, target, self.id)

    def find_elements(self, method, target):
        """Returns a list of all ``HTMLElement`` instances that match the
        specified method and target in the current context.

        For more details on this function, see the
        :func:`~marionette_driver.marionette.Marionette.find_elements` method
        in the Marionette class.
        """
        return self.marionette.find_elements(method, target, self.id)

    def get_attribute(self, name):
        """Returns the requested attribute, or None if no attribute
        is set.
        """
        body = {"id": self.id, "name": name}
        return self.marionette._send_message("WebDriver:GetElementAttribute",
                                             body, key="value")

    def get_property(self, name):
        """Returns the requested property, or None if the property is
        not set.
        """
        try:
            body = {"id": self.id, "name": name}
            return self.marionette._send_message("WebDriver:GetElementProperty",
                                                 body, key="value")
        except errors.UnknownCommandException:
            # Keep backward compatibility for code which uses get_attribute() to
            # also retrieve element properties.
            # Remove when Firefox 55 is stable.
            return self.get_attribute(name)

    def click(self):
        """Simulates a click on the element."""
        self.marionette._send_message("WebDriver:ElementClick",
                                      {"id": self.id})

    def tap(self, x=None, y=None):
        """Simulates a set of tap events on the element.

        :param x: X coordinate of tap event.  If not given, default to
            the centre of the element.
        :param y: Y coordinate of tap event. If not given, default to
            the centre of the element.
        """
        body = {"id": self.id, "x": x, "y": y}
        self.marionette._send_message("singleTap", body)

    @property
    def text(self):
        """Returns the visible text of the element, and its child elements."""
        body = {"id": self.id}
        return self.marionette._send_message("WebDriver:GetElementText",
                                             body, key="value")

    def send_keys(self, *strings):
        """Sends the string via synthesized keypresses to the element.
           If an array is passed in like `marionette.send_keys(Keys.SHIFT, "a")` it
           will be joined into a string.
           If an integer is passed in like `marionette.send_keys(1234)` it will be
           coerced into a string.
        """
        keys = Marionette.convert_keys(*strings)
        self.marionette._send_message("WebDriver:ElementSendKeys",
                                      {"id": self.id, "text": keys})

    def clear(self):
        """Clears the input of the element."""
        self.marionette._send_message("WebDriver:ElementClear",
                                      {"id": self.id})

    def is_selected(self):
        """Returns True if the element is selected."""
        body = {"id": self.id}
        return self.marionette._send_message("WebDriver:IsElementSelected",
                                             body, key="value")

    def is_enabled(self):
        """This command will return False if all the following criteria
        are met otherwise return True:

        * A form control is disabled.
        * A ``HTMLElement`` has a disabled boolean attribute.
        """
        body = {"id": self.id}
        return self.marionette._send_message("WebDriver:IsElementEnabled",
                                             body, key="value")

    def is_displayed(self):
        """Returns True if the element is displayed, False otherwise."""
        body = {"id": self.id}
        return self.marionette._send_message("WebDriver:IsElementDisplayed",
                                             body, key="value")

    @property
    def tag_name(self):
        """The tag name of the element."""
        body = {"id": self.id}
        return self.marionette._send_message("WebDriver:GetElementTagName",
                                             body, key="value")

    @property
    def rect(self):
        """Gets the element's bounding rectangle.

        This will return a dictionary with the following:

          * x and y represent the top left coordinates of the ``HTMLElement``
            relative to top left corner of the document.
          * height and the width will contain the height and the width
            of the DOMRect of the ``HTMLElement``.
        """
        return self.marionette._send_message("WebDriver:GetElementRect",
                                             {"id": self.id})

    def value_of_css_property(self, property_name):
        """Gets the value of the specified CSS property name.

        :param property_name: Property name to get the value of.
        """
        body = {"id": self.id, "propertyName": property_name}
        return self.marionette._send_message("WebDriver:GetElementCSSValue",
                                             body, key="value")


class MouseButton(object):
    """Enum-like class for mouse button constants."""
    LEFT = 0
    MIDDLE = 1
    RIGHT = 2


class Actions(object):
    '''
    An Action object represents a set of actions that are executed in a particular order.

    All action methods (press, etc.) return the Actions object itself, to make
    it easy to create a chain of events.

    Example usage:

    ::

        # get html file
        testAction = marionette.absolute_url("testFool.html")
        # navigate to the file
        marionette.navigate(testAction)
        # find element1 and element2
        element1 = marionette.find_element(By.ID, "element1")
        element2 = marionette.find_element(By.ID, "element2")
        # create action object
        action = Actions(marionette)
        # add actions (press, wait, move, release) into the object
        action.press(element1).wait(5). move(element2).release()
        # fire all the added events
        action.perform()
    '''

    def __init__(self, marionette):
        self.action_chain = []
        self.marionette = marionette
        self.current_id = None

    def press(self, element, x=None, y=None):
        '''
        Sends a 'touchstart' event to this element.

        If no coordinates are given, it will be targeted at the center of the
        element. If given, it will be targeted at the (x,y) coordinates
        relative to the top-left corner of the element.

        :param element: The element to press on.
        :param x: Optional, x-coordinate to tap, relative to the top-left
         corner of the element.
        :param y: Optional, y-coordinate to tap, relative to the top-left
         corner of the element.
        '''
        element = element.id
        self.action_chain.append(['press', element, x, y])
        return self

    def release(self):
        '''
        Sends a 'touchend' event to this element.

        May only be called if :func:`press` has already be called on this element.

        If press and release are chained without a move action between them,
        then it will be processed as a 'tap' event, and will dispatch the
        expected mouse events ('mousemove' (if necessary), 'mousedown',
        'mouseup', 'mouseclick') after the touch events. If there is a wait
        period between press and release that will trigger a contextmenu,
        then the 'contextmenu' menu event will be fired instead of the
        touch/mouse events.
        '''
        self.action_chain.append(['release'])
        return self

    def move(self, element):
        '''
        Sends a 'touchmove' event at the center of the target element.

        :param element: Element to move towards.

        May only be called if :func:`press` has already be called.
        '''
        element = element.id
        self.action_chain.append(['move', element])
        return self

    def move_by_offset(self, x, y):
        '''
        Sends 'touchmove' event to the given x, y coordinates relative to the
        top-left of the currently touched element.

        May only be called if :func:`press` has already be called.

        :param x: Specifies x-coordinate of move event, relative to the
         top-left corner of the element.
        :param y: Specifies y-coordinate of move event, relative to the
         top-left corner of the element.
        '''
        self.action_chain.append(['moveByOffset', x, y])
        return self

    def wait(self, time=None):
        '''
        Waits for specified time period.

        :param time: Time in seconds to wait. If time is None then this has no effect
                     for a single action chain. If used inside a multi-action chain,
                     then time being None indicates that we should wait for all other
                     currently executing actions that are part of the chain to complete.
        '''
        self.action_chain.append(['wait', time])
        return self

    def cancel(self):
        '''
        Sends 'touchcancel' event to the target of the original 'touchstart' event.

        May only be called if :func:`press` has already be called.
        '''
        self.action_chain.append(['cancel'])
        return self

    def tap(self, element, x=None, y=None):
        '''
        Performs a quick tap on the target element.

        :param element: The element to tap.
        :param x: Optional, x-coordinate of tap, relative to the top-left
         corner of the element. If not specified, default to center of
         element.
        :param y: Optional, y-coordinate of tap, relative to the top-left
         corner of the element. If not specified, default to center of
         element.

        This is equivalent to calling:

        ::

          action.press(element, x, y).release()
        '''
        element = element.id
        self.action_chain.append(['press', element, x, y])
        self.action_chain.append(['release'])
        return self

    def double_tap(self, element, x=None, y=None):
        '''
        Performs a double tap on the target element.

        :param element: The element to double tap.
        :param x: Optional, x-coordinate of double tap, relative to the
         top-left corner of the element.
        :param y: Optional, y-coordinate of double tap, relative to the
         top-left corner of the element.
        '''
        element = element.id
        self.action_chain.append(['press', element, x, y])
        self.action_chain.append(['release'])
        self.action_chain.append(['press', element, x, y])
        self.action_chain.append(['release'])
        return self

    def click(self, element, button=MouseButton.LEFT, count=1):
        '''
        Performs a click with additional parameters to allow for double clicking,
        right click, middle click, etc.

        :param element: The element to click.
        :param button: The mouse button to click (indexed from 0, left to right).
        :param count: Optional, the count of clicks to synthesize (for double
                      click events).
        '''
        el = element.id
        self.action_chain.append(['click', el, button, count])
        return self

    def context_click(self, element):
        '''
        Performs a context click on the specified element.

        :param element: The element to context click.
        '''
        return self.click(element, button=MouseButton.RIGHT)

    def middle_click(self, element):
        '''
        Performs a middle click on the specified element.

        :param element: The element to middle click.
        '''
        return self.click(element, button=MouseButton.MIDDLE)

    def double_click(self, element):
        '''
        Performs a double click on the specified element.

        :param element: The element to double click.
        '''
        return self.click(element, count=2)

    def flick(self, element, x1, y1, x2, y2, duration=200):
        '''
        Performs a flick gesture on the target element.

        :param element: The element to perform the flick gesture on.
        :param x1: Starting x-coordinate of flick, relative to the top left
         corner of the element.
        :param y1: Starting y-coordinate of flick, relative to the top left
         corner of the element.
        :param x2: Ending x-coordinate of flick, relative to the top left
         corner of the element.
        :param y2: Ending y-coordinate of flick, relative to the top left
         corner of the element.
        :param duration: Time needed for the flick gesture for complete (in
         milliseconds).
        '''
        element = element.id
        elapsed = 0
        time_increment = 10
        if time_increment >= duration:
            time_increment = duration
        move_x = time_increment*1.0/duration * (x2 - x1)
        move_y = time_increment*1.0/duration * (y2 - y1)
        self.action_chain.append(['press', element, x1, y1])
        while elapsed < duration:
            elapsed += time_increment
            self.action_chain.append(['moveByOffset', move_x, move_y])
            self.action_chain.append(['wait', time_increment/1000])
        self.action_chain.append(['release'])
        return self

    def long_press(self, element, time_in_seconds, x=None, y=None):
        '''
        Performs a long press gesture on the target element.

        :param element: The element to press.
        :param time_in_seconds: Time in seconds to wait before releasing the press.
        :param x: Optional, x-coordinate to tap, relative to the top-left
         corner of the element.
        :param y: Optional, y-coordinate to tap, relative to the top-left
         corner of the element.

        This is equivalent to calling:

        ::

          action.press(element, x, y).wait(time_in_seconds).release()

        '''
        element = element.id
        self.action_chain.append(['press', element, x, y])
        self.action_chain.append(['wait', time_in_seconds])
        self.action_chain.append(['release'])
        return self

    def key_down(self, key_code):
        """
        Perform a "keyDown" action for the given key code. Modifier keys are
        respected by the server for the course of an action chain.

        :param key_code: The key to press as a result of this action.
        """
        self.action_chain.append(['keyDown', key_code])
        return self

    def key_up(self, key_code):
        """
        Perform a "keyUp" action for the given key code. Modifier keys are
        respected by the server for the course of an action chain.

        :param key_up: The key to release as a result of this action.
        """
        self.action_chain.append(['keyUp', key_code])
        return self

    def perform(self):
        """Sends the action chain built so far to the server side for
        execution and clears the current chain of actions."""
        body = {"chain": self.action_chain, "nextId": self.current_id}
        try:
            self.current_id = self.marionette._send_message("Marionette:ActionChain",
                                                            body, key="value")
        except errors.UnknownCommandException:
            self.current_id = self.marionette._send_message("actionChain",
                                                            body, key="value")
        self.action_chain = []
        return self


class MultiActions(object):
    '''
    A MultiActions object represents a sequence of actions that may be
    performed at the same time. Its intent is to allow the simulation
    of multi-touch gestures.
    Usage example:

    ::

      # create multiaction object
      multitouch = MultiActions(marionette)
      # create several action objects
      action_1 = Actions(marionette)
      action_2 = Actions(marionette)
      # add actions to each action object/finger
      action_1.press(element1).move_to(element2).release()
      action_2.press(element3).wait().release(element3)
      # fire all the added events
      multitouch.add(action_1).add(action_2).perform()
    '''

    def __init__(self, marionette):
        self.multi_actions = []
        self.max_length = 0
        self.marionette = marionette

    def add(self, action):
        '''
        Adds a set of actions to perform.

        :param action: An Actions object.
        '''
        self.multi_actions.append(action.action_chain)
        if len(action.action_chain) > self.max_length:
            self.max_length = len(action.action_chain)
        return self

    def perform(self):
        """Perform all the actions added to this object."""
        body = {"value": self.multi_actions, "max_length": self.max_length}
        try:
            self.marionette._send_message("Marionette:MultiAction", body)
        except errors.UnknownCommandException:
            self.marionette._send_message("multiAction", body)


class Alert(object):
    """A class for interacting with alerts.

    ::

        Alert(marionette).accept()
        Alert(marionette).dismiss()
    """

    def __init__(self, marionette):
        self.marionette = marionette

    def accept(self):
        """Accept a currently displayed modal dialog."""
        # TODO: Switch to "WebDriver:AcceptAlert" in Firefox 62
        self.marionette._send_message("WebDriver:AcceptDialog")

    def dismiss(self):
        """Dismiss a currently displayed modal dialog."""
        self.marionette._send_message("WebDriver:DismissAlert")

    @property
    def text(self):
        """Return the currently displayed text in a tab modal."""
        return self.marionette._send_message("WebDriver:GetAlertText",
                                             key="value")

    def send_keys(self, *string):
        """Send keys to the currently displayed text input area in an open
        tab modal dialog."""
        self.marionette._send_message("WebDriver:SendAlertText",
                                      {"text": Marionette.convert_keys(*string)})


class Marionette(object):
    """Represents a Marionette connection to a browser or device."""

    CONTEXT_CHROME = "chrome"  # non-browser content: windows, dialogs, etc.
    CONTEXT_CONTENT = "content"  # browser content: iframes, divs, etc.
    DEFAULT_STARTUP_TIMEOUT = 120
    DEFAULT_SHUTDOWN_TIMEOUT = 120  # Firefox will kill hanging threads after 60s

    # Bug 1336953 - Until we can remove the socket timeout parameter it has to be
    # set a default value which is larger than the longest timeout as defined by the
    # WebDriver spec. In that case its 300s for page load. Also add another minute
    # so that slow builds have enough time to send the timeout error to the client.
    DEFAULT_SOCKET_TIMEOUT = 360

    def __init__(self, host="localhost", port=2828, app=None, bin=None,
                 baseurl=None, socket_timeout=None,
                 startup_timeout=None, **instance_args):
        """Construct a holder for the Marionette connection.

        Remember to call ``start_session`` in order to initiate the
        connection and start a Marionette session.

        :param host: Host where the Marionette server listens.
            Defaults to localhost.
        :param port: Port where the Marionette server listens.
            Defaults to port 2828.
        :param baseurl: Where to look for files served from Marionette's
            www directory.
        :param socket_timeout: Timeout for Marionette socket operations.
        :param startup_timeout: Seconds to wait for a connection with
            binary.
        :param bin: Path to browser binary.  If any truthy value is given
            this will attempt to start a Gecko instance with the specified
            `app`.
        :param app: Type of ``instance_class`` to use for managing app
            instance. See ``marionette_driver.geckoinstance``.
        :param instance_args: Arguments to pass to ``instance_class``.

        """
        self.host = host
        self.port = self.local_port = int(port)
        self.bin = bin
        self.client = None
        self.instance = None
        self.session = None
        self.session_id = None
        self.process_id = None
        self.profile = None
        self.window = None
        self.chrome_window = None
        self.baseurl = baseurl
        self._test_name = None
        self.crashed = 0

        if socket_timeout is None:
            self.socket_timeout = self.DEFAULT_SOCKET_TIMEOUT
        else:
            self.socket_timeout = float(socket_timeout)

        if startup_timeout is None:
            self.startup_timeout = self.DEFAULT_STARTUP_TIMEOUT
        else:
            self.startup_timeout = int(startup_timeout)

        if self.bin:
            self.instance = GeckoInstance.create(
                app, host=self.host, port=self.port, bin=self.bin, **instance_args)
            self.start_binary(self.startup_timeout)

        self.timeout = Timeouts(self)

    @property
    def profile_path(self):
        if self.instance and self.instance.profile:
            return self.instance.profile.profile

    def start_binary(self, timeout):
        if not self.is_port_available(self.port, host=self.host):
            raise IOError("Port {0}:{1} is unavailable.".format(self.host, self.port))

        try:
            self.instance.start()
            self.raise_for_port(timeout=timeout)
        except socket.timeout:
            # Something went wrong with starting up Marionette server. Given
            # that the process will not quit itself, force a shutdown immediately.
            self.cleanup()

            msg = "Process killed after {}s because no connection to Marionette "\
                  "server could be established. Check gecko.log for errors"
            _, _, tb = sys.exc_info()
            reraise(IOError, msg.format(timeout), tb)

    def cleanup(self):
        if self.session is not None:
            try:
                self.delete_session()
            except (errors.MarionetteException, IOError):
                # These exceptions get thrown if the Marionette server
                # hit an exception/died or the connection died. We can
                # do no further server-side cleanup in this case.
                pass
        if self.instance:
            # stop application and, if applicable, stop emulator
            self.instance.close(clean=True)
            if self.instance.unresponsive_count >= 3:
                raise errors.UnresponsiveInstanceException(
                    "Application clean-up has failed >2 consecutive times.")

    def __del__(self):
        self.cleanup()

    @staticmethod
    def is_port_available(port, host=''):
        port = int(port)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return True
        except socket.error:
            return False
        finally:
            s.close()

    def raise_for_port(self, timeout=None):
        """Raise socket.timeout if no connection can be established.

        :param timeout: Timeout in seconds for the server to be ready.

        """
        if timeout is None:
            timeout = self.DEFAULT_STARTUP_TIMEOUT

        runner = None
        if self.instance is not None:
            runner = self.instance.runner

        poll_interval = 0.1
        starttime = datetime.datetime.now()
        timeout_time = starttime + datetime.timedelta(seconds=timeout)

        client = transport.TcpTransport(self.host, self.port, 0.5)

        connected = False
        while datetime.datetime.now() < timeout_time:
            # If the instance we want to connect to is not running return immediately
            if runner is not None and not runner.is_running():
                break

            try:
                client.connect()
                return True
            except socket.error:
                pass
            finally:
                client.close()

            time.sleep(poll_interval)

        if not connected:
            raise socket.timeout("Timed out waiting for connection on {0}:{1}!".format(
                self.host, self.port))

    @do_process_check
    def _send_message(self, name, params=None, key=None):
        """Send a blocking message to the server.

        Marionette provides an asynchronous, non-blocking interface and
        this attempts to paper over this by providing a synchronous API
        to the user.

        :param name: Requested command key.
        :param params: Optional dictionary of key/value arguments.
        :param key: Optional key to extract from response.

        :returns: Full response from the server, or if `key` is given,
            the value of said key in the response.
        """

        if not self.session_id and name != "WebDriver:NewSession":
            raise errors.MarionetteException("Please start a session")

        try:
            msg = self.client.request(name, params)

        except IOError:
            self.delete_session(send_request=False)
            raise

        res, err = msg.result, msg.error
        if err:
            self._handle_error(err)

        if key is not None:
            return self._unwrap_response(res.get(key))
        else:
            return self._unwrap_response(res)

    def _unwrap_response(self, value):
        if isinstance(value, dict) and (WEBELEMENT_KEY in value or
                                        W3C_WEBELEMENT_KEY in value):
            if value.get(WEBELEMENT_KEY):
                return HTMLElement(self, value.get(WEBELEMENT_KEY))
            else:
                return HTMLElement(self, value.get(W3C_WEBELEMENT_KEY))
        elif isinstance(value, list):
            return list(self._unwrap_response(item) for item in value)
        else:
            return value

    def _handle_error(self, obj):
        error = obj["error"]
        message = obj["message"]
        stacktrace = obj["stacktrace"]

        raise errors.lookup(error)(message, stacktrace=stacktrace)

    def check_for_crash(self):
        """Check if the process crashed.

        :returns: True, if a crash happened since the method has been called the last time.
        """
        crash_count = 0

        if self.instance:
            name = self.test_name or 'marionette.py'
            crash_count = self.instance.runner.check_for_crashes(test_name=name)
            self.crashed = self.crashed + crash_count

        return crash_count > 0

    def _handle_socket_failure(self):
        """Handle socket failures for the currently connected application.

        If the application crashed then clean-up internal states, or in case of a content
        crash also kill the process. If there are other reasons for a socket failure,
        wait for the process to shutdown itself, or force kill it.

        Please note that the method expects an exception to be handled on the current stack
        frame, and is only called via the `@do_process_check` decorator.

        """
        exc, val, tb = sys.exc_info()

        # If the application hasn't been launched by Marionette no further action can be done.
        # In such cases we simply re-throw the exception.
        if not self.instance:
            reraise(exc, val, tb)

        else:
            # Somehow the socket disconnected. Give the application some time to shutdown
            # itself before killing the process.
            returncode = self.instance.runner.wait(timeout=self.DEFAULT_SHUTDOWN_TIMEOUT)

            if returncode is None:
                message = ('Process killed because the connection to Marionette server is '
                           'lost. Check gecko.log for errors')
                # This will force-close the application without sending any other message.
                self.cleanup()
            else:
                # If Firefox quit itself check if there was a crash
                crash_count = self.check_for_crash()

                if crash_count > 0:
                    if returncode == 0:
                        message = 'Content process crashed'
                    else:
                        message = 'Process crashed (Exit code: {returncode})'
                else:
                    message = 'Process has been unexpectedly closed (Exit code: {returncode})'

                self.delete_session(send_request=False)

            message += ' (Reason: {reason})'

            reraise(IOError, message.format(returncode=returncode, reason=val), tb)

    @staticmethod
    def convert_keys(*string):
        typing = []
        for val in string:
            if isinstance(val, Keys):
                typing.append(val)
            elif isinstance(val, int):
                val = str(val)
                for i in range(len(val)):
                    typing.append(val[i])
            else:
                for i in range(len(val)):
                    typing.append(val[i])
        return "".join(typing)

    def clear_pref(self, pref):
        """Clear the user-defined value from the specified preference.

        :param pref: Name of the preference.
        """
        with self.using_context(self.CONTEXT_CHROME):
            self.execute_script("""
               Components.utils.import("resource://gre/modules/Preferences.jsm");
               Preferences.reset(arguments[0]);
               """, script_args=(pref,))

    def get_pref(self, pref, default_branch=False, value_type="unspecified"):
        """Get the value of the specified preference.

        :param pref: Name of the preference.
        :param default_branch: Optional, if `True` the preference value will be read
                               from the default branch. Otherwise the user-defined
                               value if set is returned. Defaults to `False`.
        :param value_type: Optional, XPCOM interface of the pref's complex value.
                           Possible values are: `nsIFile` and
                           `nsIPrefLocalizedString`.

        Usage example::

            marionette.get_pref("browser.tabs.warnOnClose")

        """
        with self.using_context(self.CONTEXT_CHROME):
            pref_value = self.execute_script("""
                Components.utils.import("resource://gre/modules/Preferences.jsm");

                let pref = arguments[0];
                let defaultBranch = arguments[1];
                let valueType = arguments[2];

                prefs = new Preferences({defaultBranch: defaultBranch});
                return prefs.get(pref, null, Components.interfaces[valueType]);
                """, script_args=(pref, default_branch, value_type))
            return pref_value

    def set_pref(self, pref, value, default_branch=False):
        """Set the value of the specified preference.

        :param pref: Name of the preference.
        :param value: The value to set the preference to. If the value is None,
                      reset the preference to its default value. If no default
                      value exists, the preference will cease to exist.
        :param default_branch: Optional, if `True` the preference value will
                       be written to the default branch, and will remain until
                       the application gets restarted. Otherwise a user-defined
                       value is set. Defaults to `False`.

        Usage example::

            marionette.set_pref("browser.tabs.warnOnClose", True)

        """
        with self.using_context(self.CONTEXT_CHROME):
            if value is None:
                self.clear_pref(pref)
                return

            self.execute_script("""
                Components.utils.import("resource://gre/modules/Preferences.jsm");

                let pref = arguments[0];
                let value = arguments[1];
                let defaultBranch = arguments[2];

                prefs = new Preferences({defaultBranch: defaultBranch});
                prefs.set(pref, value);
                """, script_args=(pref, value, default_branch))

    def set_prefs(self, prefs, default_branch=False):
        """Set the value of a list of preferences.

        :param prefs: A dict containing one or more preferences and their values
                      to be set. See :func:`set_pref` for further details.
        :param default_branch: Optional, if `True` the preference value will
                       be written to the default branch, and will remain until
                       the application gets restarted. Otherwise a user-defined
                       value is set. Defaults to `False`.

        Usage example::

            marionette.set_prefs({"browser.tabs.warnOnClose": True})

        """
        for pref, value in prefs.items():
            self.set_pref(pref, value, default_branch=default_branch)

    @contextmanager
    def using_prefs(self, prefs, default_branch=False):
        """Set preferences for code executed in a `with` block, and restores them on exit.

        :param prefs: A dict containing one or more preferences and their values
                      to be set. See :func:`set_prefs` for further details.
        :param default_branch: Optional, if `True` the preference value will
                       be written to the default branch, and will remain until
                       the application gets restarted. Otherwise a user-defined
                       value is set. Defaults to `False`.

        Usage example::

            with marionette.using_prefs({"browser.tabs.warnOnClose": True}):
                # ... do stuff ...

        """
        original_prefs = {p: self.get_pref(p) for p in prefs}
        self.set_prefs(prefs, default_branch=default_branch)

        try:
            yield
        finally:
            self.set_prefs(original_prefs, default_branch=default_branch)

    @do_process_check
    def enforce_gecko_prefs(self, prefs):
        """Checks if the running instance has the given prefs. If not,
        it will kill the currently running instance, and spawn a new
        instance with the requested preferences.

        :param prefs: A dictionary whose keys are preference names.
        """
        if not self.instance:
            raise errors.MarionetteException("enforce_gecko_prefs() can only be called "
                                             "on Gecko instances launched by Marionette")
        pref_exists = True
        with self.using_context(self.CONTEXT_CHROME):
            for pref, value in prefs.iteritems():
                if type(value) is not str:
                    value = json.dumps(value)
                pref_exists = self.execute_script("""
                let prefInterface = Components.classes["@mozilla.org/preferences-service;1"]
                                              .getService(Components.interfaces.nsIPrefBranch);
                let pref = '{0}';
                let value = '{1}';
                let type = prefInterface.getPrefType(pref);
                switch(type) {{
                    case prefInterface.PREF_STRING:
                        return value == prefInterface.getCharPref(pref).toString();
                    case prefInterface.PREF_BOOL:
                        return value == prefInterface.getBoolPref(pref).toString();
                    case prefInterface.PREF_INT:
                        return value == prefInterface.getIntPref(pref).toString();
                    case prefInterface.PREF_INVALID:
                        return false;
                }}
                """.format(pref, value))
                if not pref_exists:
                    break

        if not pref_exists:
            context = self._send_message("Marionette:GetContext",
                                         key="value")
            self.delete_session()
            self.instance.restart(prefs)
            self.raise_for_port()
            self.start_session()

            # Restore the context as used before the restart
            self.set_context(context)

    def _request_in_app_shutdown(self, *shutdown_flags):
        """Attempt to quit the currently running instance from inside the
        application.

        Duplicate entries in `shutdown_flags` are removed, and
        `"eForceQuit"` is added if no other `*Quit` flags are given.
        This provides backwards compatible behaviour with earlier
        Firefoxen.

        This method effectively calls `Services.startup.quit` in Gecko.
        Possible flag values are listed at http://mzl.la/1X0JZsC.

        :param shutdown_flags: Optional additional quit masks to include.
            Duplicates are removed, and `"eForceQuit"` is added if no
            flags ending with `"Quit"` are present.

        :throws InvalidArgumentException: If there are multiple
            `shutdown_flags` ending with `"Quit"`.

        :returns: The cause of shutdown.
        """

        # The vast majority of this function was implemented inside
        # the quit command as part of bug 1337743, and can be
        # removed from here in Firefox 55 at the earliest.

        # remove duplicates
        flags = set(shutdown_flags)

        # add eForceQuit if there are no *Quits
        if not any(flag.endswith("Quit") for flag in flags):
            flags = flags | set(("eForceQuit",))

        # Trigger a quit-application-requested observer notification
        # so that components can safely shutdown before quitting the
        # application.
        with self.using_context("chrome"):
            canceled = self.execute_script("""
                Components.utils.import("resource://gre/modules/Services.jsm");
                let cancelQuit = Components.classes["@mozilla.org/supports-PRBool;1"]
                    .createInstance(Components.interfaces.nsISupportsPRBool);
                Services.obs.notifyObservers(cancelQuit, "quit-application-requested", null);
                return cancelQuit.data;
                """)
            if canceled:
                raise errors.MarionetteException(
                    "Something cancelled the quit application request")

        body = None
        if len(flags) > 0:
            body = {"flags": list(flags)}

        return self._send_message("Marionette:Quit",
                                  body, key="cause")

    @do_process_check
    def quit(self, clean=False, in_app=False, callback=None):
        """Terminate the currently running instance.

        This command will delete the active marionette session. It also allows
        manipulation of eg. the profile data while the application is not running.
        To start the application again, :func:`start_session` has to be called.

        :param clean: If False the same profile will be used after the next start of
                      the application. Note that the in app initiated restart always
                      maintains the same profile.
        :param in_app: If True, marionette will cause a quit from within the
                       browser. Otherwise the browser will be quit immediately
                       by killing the process.
        :param callback: If provided and `in_app` is True, the callback will
                         be used to trigger the shutdown.
        """
        if not self.instance:
            raise errors.MarionetteException("quit() can only be called "
                                             "on Gecko instances launched by Marionette")

        cause = None
        if in_app:
            if callback is not None:
                if not callable(callback):
                    raise ValueError("Specified callback '{}' is not callable".format(callback))

                self._send_message("Marionette:AcceptConnections",
                                   {"value": False})
                callback()
            else:
                cause = self._request_in_app_shutdown()

            self.delete_session(send_request=False)

            # Give the application some time to shutdown
            returncode = self.instance.runner.wait(timeout=self.DEFAULT_SHUTDOWN_TIMEOUT)
            if returncode is None:
                # This will force-close the application without sending any other message.
                self.cleanup()

                message = ("Process killed because a requested application quit did not happen "
                           "within {}s. Check gecko.log for errors.")
                raise IOError(message.format(self.DEFAULT_SHUTDOWN_TIMEOUT))

        else:
            self.delete_session()
            self.instance.close(clean=clean)

        if cause not in (None, "shutdown"):
            raise errors.MarionetteException("Unexpected shutdown reason '{}' for "
                                             "quitting the process.".format(cause))

    @do_process_check
    def restart(self, clean=False, in_app=False, callback=None):
        """
        This will terminate the currently running instance, and spawn a new instance
        with the same profile and then reuse the session id when creating a session again.

        :param clean: If False the same profile will be used after the restart. Note
                      that the in app initiated restart always maintains the same
                      profile.
        :param in_app: If True, marionette will cause a restart from within the
                       browser. Otherwise the browser will be restarted immediately
                       by killing the process.
        :param callback: If provided and `in_app` is True, the callback will be
                         used to trigger the restart.
        """
        if not self.instance:
            raise errors.MarionetteException("restart() can only be called "
                                             "on Gecko instances launched by Marionette")
        context = self._send_message("Marionette:GetContext",
                                     key="value")

        cause = None
        if in_app:
            if clean:
                raise ValueError("An in_app restart cannot be triggered with the clean flag set")

            if callback is not None:
                if not callable(callback):
                    raise ValueError("Specified callback '{}' is not callable".format(callback))

                self._send_message("Marionette:AcceptConnections",
                                   {"value": False})
                callback()
            else:
                cause = self._request_in_app_shutdown("eRestart")

            self.delete_session(send_request=False)

            try:
                timeout = self.DEFAULT_SHUTDOWN_TIMEOUT + self.DEFAULT_STARTUP_TIMEOUT
                self.raise_for_port(timeout=timeout)
            except socket.timeout:
                if self.instance.runner.returncode is not None:
                    exc, val, tb = sys.exc_info()
                    self.cleanup()
                    reraise(exc, "Requested restart of the application was aborted", tb)

        else:
            self.delete_session()
            self.instance.restart(clean=clean)
            self.raise_for_port(timeout=self.DEFAULT_STARTUP_TIMEOUT)

        if cause not in (None, "restart"):
            raise errors.MarionetteException("Unexpected shutdown reason '{}' for "
                                             "restarting the process".format(cause))

        self.start_session()
        # Restore the context as used before the restart
        self.set_context(context)

        if in_app and self.process_id:
            # In some cases Firefox restarts itself by spawning into a new process group.
            # As long as mozprocess cannot track that behavior (bug 1284864) we assist by
            # informing about the new process id.
            self.instance.runner.process_handler.check_for_detached(self.process_id)

    def absolute_url(self, relative_url):
        '''
        Returns an absolute url for files served from Marionette's www directory.

        :param relative_url: The url of a static file, relative to Marionette's www directory.
        '''
        return "{0}{1}".format(self.baseurl, relative_url)

    @do_process_check
    def start_session(self, capabilities=None, timeout=None):
        """Create a new WebDriver session.
        This method must be called before performing any other action.

        :param capabilities: An optional dictionary of
            Marionette-recognised capabilities.  It does not
            accept a WebDriver conforming capabilities dictionary
            (including alwaysMatch, firstMatch, desiredCapabilities,
            or requriedCapabilities), and only recognises extension
            capabilities that are specific to Marionette.
        :param timeout: Optional timeout in seconds for the server to be ready.
        :returns: A dictionary of the capabilities offered.
        """
        if timeout is None:
            timeout = self.startup_timeout

        self.crashed = 0

        if self.instance:
            returncode = self.instance.runner.returncode
            # We're managing a binary which has terminated. Start it again
            # and implicitely wait for the Marionette server to be ready.
            if returncode is not None:
                self.start_binary(timeout)

        else:
            # In the case when Marionette doesn't manage the binary wait until
            # its server component has been started.
            self.raise_for_port(timeout=timeout)

        self.client = transport.TcpTransport(
            self.host,
            self.port,
            self.socket_timeout)
        self.protocol, _ = self.client.connect()

        body = capabilities
        if body is None:
            body = {}

        # Duplicate capabilities object so the body we end up
        # sending looks like this:
        #
        #     {acceptInsecureCerts: true, {capabilities: {acceptInsecureCerts: true}}}
        #
        # We do this because geckodriver sends the capabilities at the
        # top-level, and after bug 1388424 removed support for overriding
        # the session ID, we also do this with this client.  However,
        # because this client is used with older Firefoxen (through upgrade
        # tests et al.) we need to preserve backwards compatibility until
        # Firefox 60.
        if "capabilities" not in body and capabilities is not None:
            body["capabilities"] = dict(capabilities)

        resp = self._send_message("WebDriver:NewSession",
                                  body)
        self.session_id = resp["sessionId"]
        self.session = resp["capabilities"]
        # fallback to processId can be removed in Firefox 55
        self.process_id = self.session.get("moz:processID", self.session.get("processId"))
        self.profile = self.session.get("moz:profile")

        return self.session

    @property
    def test_name(self):
        return self._test_name

    @test_name.setter
    def test_name(self, test_name):
        self._test_name = test_name

    def delete_session(self, send_request=True):
        """Close the current session and disconnect from the server.

        :param send_request: Optional, if `True` a request to close the session on
            the server side will be sent. Use `False` in case of eg. in_app restart()
            or quit(), which trigger a deletion themselves. Defaults to `True`.
        """
        try:
            if send_request:
                self._send_message("WebDriver:DeleteSession")
        finally:
            self.process_id = None
            self.profile = None
            self.session = None
            self.session_id = None
            self.window = None

            if self.client is not None:
                self.client.close()

    @property
    def session_capabilities(self):
        """A JSON dictionary representing the capabilities of the
        current session.

        """
        return self.session

    @property
    def current_window_handle(self):
        """Get the current window's handle.

        Returns an opaque server-assigned identifier to this window
        that uniquely identifies it within this Marionette instance.
        This can be used to switch to this window at a later point.

        :returns: unique window handle
        :rtype: string
        """
        self.window = self._send_message("WebDriver:GetWindowHandle",
                                         key="value")
        return self.window

    @property
    def current_chrome_window_handle(self):
        """Get the current chrome window's handle. Corresponds to
        a chrome window that may itself contain tabs identified by
        window_handles.

        Returns an opaque server-assigned identifier to this window
        that uniquely identifies it within this Marionette instance.
        This can be used to switch to this window at a later point.

        :returns: unique window handle
        :rtype: string
        """
        self.chrome_window = self._send_message("WebDriver:GetChromeWindowHandle",
                                                key="value")

        return self.chrome_window

    def get_window_position(self):
        """Get the current window's position.

        :returns: a dictionary with x and y
        """
        warnings.warn("get_window_position() has been deprecated, please use get_window_rect()",
                      DeprecationWarning)
        return self._send_message("getWindowPosition")

    def set_window_position(self, x, y):
        """Set the position of the current window

        :param x: x coordinate for the top left of the window
        :param y: y coordinate for the top left of the window
        """
        warnings.warn("set_window_position() has been deprecated, please use set_window_rect()",
                      DeprecationWarning)
        self._send_message("setWindowPosition", {"x": x, "y": y})

    def set_window_rect(self, x=None, y=None, height=None, width=None):
        """Set the position and size of the current window.

        The supplied width and height values refer to the window outerWidth
        and outerHeight values, which include scroll bars, title bars, etc.

        An error will be returned if the requested window size would result
        in the window being in the maximised state.

        :param x: x coordinate for the top left of the window
        :param y: y coordinate for the top left of the window
        :param width: The width to resize the window to.
        :param height: The height to resize the window to.
        """
        if (x is None and y is None) and (height is None and width is None):
            raise errors.InvalidArgumentException("x and y or height and width need values")

        body = {"x": x, "y": y, "height": height, "width": width}
        return self._send_message("WebDriver:SetWindowRect",
                                  body)

    @property
    def window_rect(self):
        return self._send_message("WebDriver:GetWindowRect")

    @property
    def title(self):
        """Current title of the active window."""
        return self._send_message("WebDriver:GetTitle",
                                  key="value")

    @property
    def window_handles(self):
        """Get list of windows in the current context.

        If called in the content context it will return a list of
        references to all available browser windows.  Called in the
        chrome context, it will list all available windows, not just
        browser windows (e.g. not just navigator.browser).

        Each window handle is assigned by the server, and the list of
        strings returned does not have a guaranteed ordering.

        :returns: Unordered list of unique window handles as strings
        """
        return self._send_message("WebDriver:GetWindowHandles")

    @property
    def chrome_window_handles(self):
        """Get a list of currently open chrome windows.

        Each window handle is assigned by the server, and the list of
        strings returned does not have a guaranteed ordering.

        :returns: Unordered list of unique chrome window handles as strings
        """
        return self._send_message("WebDriver:GetChromeWindowHandles")

    @property
    def page_source(self):
        """A string representation of the DOM."""
        return self._send_message("WebDriver:GetPageSource",
                                  key="value")

    def close(self):
        """Close the current window, ending the session if it's the last
        window currently open.

        :returns: Unordered list of remaining unique window handles as strings
        """
        return self._send_message("WebDriver:CloseWindow")

    def close_chrome_window(self):
        """Close the currently selected chrome window, ending the session
        if it's the last window open.

        :returns: Unordered list of remaining unique chrome window handles as strings
        """
        return self._send_message("WebDriver:CloseChromeWindow")

    def set_context(self, context):
        """Sets the context that Marionette commands are running in.

        :param context: Context, may be one of the class properties
            `CONTEXT_CHROME` or `CONTEXT_CONTENT`.

        Usage example::

            marionette.set_context(marionette.CONTEXT_CHROME)
        """
        if context not in [self.CONTEXT_CHROME, self.CONTEXT_CONTENT]:
            raise ValueError("Unknown context: {}".format(context))

        self._send_message("Marionette:SetContext",
                           {"value": context})

    @contextmanager
    def using_context(self, context):
        """Sets the context that Marionette commands are running in using
        a `with` statement. The state of the context on the server is
        saved before entering the block, and restored upon exiting it.

        :param context: Context, may be one of the class properties
            `CONTEXT_CHROME` or `CONTEXT_CONTENT`.

        Usage example::

            with marionette.using_context(marionette.CONTEXT_CHROME):
                # chrome scope
                ... do stuff ...
        """
        scope = self._send_message("Marionette:GetContext",
                                   key="value")
        self.set_context(context)
        try:
            yield
        finally:
            self.set_context(scope)

    def switch_to_alert(self):
        """Returns an :class:`~marionette_driver.marionette.Alert` object for
        interacting with a currently displayed alert.

        ::

            alert = self.marionette.switch_to_alert()
            text = alert.text
            alert.accept()
        """
        return Alert(self)

    def switch_to_window(self, window_id, focus=True):
        """Switch to the specified window; subsequent commands will be
        directed at the new window.

        :param window_id: The id or name of the window to switch to.

        :param focus: A boolean value which determins whether to focus
            the window that we just switched to.
        """
        self._send_message("WebDriver:SwitchToWindow",
                           {"focus": focus, "name": window_id})
        self.window = window_id

    def get_active_frame(self):
        """Returns an :class:`~marionette_driver.marionette.HTMLElement`
        representing the frame Marionette is currently acting on."""
        return self._send_message("WebDriver:GetActiveFrame",
                                  key="value")

    def switch_to_default_content(self):
        """Switch the current context to page's default content."""
        return self.switch_to_frame()

    def switch_to_parent_frame(self):
        """
           Switch to the Parent Frame
        """
        self._send_message("WebDriver:SwitchToParentFrame")

    def switch_to_frame(self, frame=None, focus=True):
        """Switch the current context to the specified frame. Subsequent
        commands will operate in the context of the specified frame,
        if applicable.

        :param frame: A reference to the frame to switch to.  This can
            be an :class:`~marionette_driver.marionette.HTMLElement`,
            an integer index, string name, or an
            ID attribute.  If you call ``switch_to_frame`` without an
            argument, it will switch to the top-level frame.

        :param focus: A boolean value which determins whether to focus
            the frame that we just switched to.
        """
        body = {"focus": focus}
        if isinstance(frame, HTMLElement):
            body["element"] = frame.id
        elif frame is not None:
            body["id"] = frame

        self._send_message("WebDriver:SwitchToFrame",
                           body)

    def switch_to_shadow_root(self, host=None):
        """Switch the current context to the specified host's Shadow DOM.
        Subsequent commands will operate in the context of the specified Shadow
        DOM, if applicable.

        :param host: A reference to the host element containing Shadow DOM.
            This can be an :class:`~marionette_driver.marionette.HTMLElement`.
            If you call ``switch_to_shadow_root`` without an argument, it will
            switch to the parent Shadow DOM or the top-level frame.
        """
        body = {}
        if isinstance(host, HTMLElement):
            body["id"] = host.id

        return self._send_message("WebDriver:SwitchToShadowRoot",
                                  body)

    def get_url(self):
        """Get a string representing the current URL.

        On Desktop this returns a string representation of the URL of
        the current top level browsing context.  This is equivalent to
        document.location.href.

        When in the context of the chrome, this returns the canonical
        URL of the current resource.

        :returns: string representation of URL
        """
        return self._send_message("WebDriver:GetCurrentURL",
                                  key="value")

    def get_window_type(self):
        """Gets the windowtype attribute of the window Marionette is
        currently acting on.

        This command only makes sense in a chrome context. You might use this
        method to distinguish a browser window from an editor window.
        """
        try:
            return self._send_message("Marionette:GetWindowType",
                                      key="value")
        except errors.UnknownCommandException:
            return self._send_message("getWindowType", key="value")

    def navigate(self, url):
        """Navigate to given `url`.

        Navigates the current top-level browsing context's content
        frame to the given URL and waits for the document to load or
        the session's page timeout duration to elapse before returning.

        The command will return with a failure if there is an error
        loading the document or the URL is blocked.  This can occur if
        it fails to reach the host, the URL is malformed, the page is
        restricted (about:* pages), or if there is a certificate issue
        to name some examples.

        The document is considered successfully loaded when the
        `DOMContentLoaded` event on the frame element associated with the
        `window` triggers and `document.readyState` is "complete".

        In chrome context it will change the current `window`'s location
        to the supplied URL and wait until `document.readyState` equals
        "complete" or the page timeout duration has elapsed.

        :param url: The URL to navigate to.
        """
        self._send_message("WebDriver:Navigate",
                           {"url": url})

    def go_back(self):
        """Causes the browser to perform a back navigation."""
        self._send_message("WebDriver:Back")

    def go_forward(self):
        """Causes the browser to perform a forward navigation."""
        self._send_message("WebDriver:Forward")

    def refresh(self):
        """Causes the browser to perform to refresh the current page."""
        self._send_message("WebDriver:Refresh")

    def _to_json(self, args):
        if isinstance(args, list) or isinstance(args, tuple):
            wrapped = []
            for arg in args:
                wrapped.append(self._to_json(arg))
        elif isinstance(args, dict):
            wrapped = {}
            for arg in args:
                wrapped[arg] = self._to_json(args[arg])
        elif type(args) == HTMLElement:
            wrapped = {W3C_WEBELEMENT_KEY: args.id,
                       WEBELEMENT_KEY: args.id}
        elif (isinstance(args, bool) or isinstance(args, basestring) or
              isinstance(args, int) or isinstance(args, float) or args is None):
            wrapped = args
        return wrapped

    def _from_json(self, value):
        if isinstance(value, list):
            unwrapped = []
            for item in value:
                unwrapped.append(self._from_json(item))
        elif isinstance(value, dict):
            unwrapped = {}
            for key in value:
                if key == W3C_WEBELEMENT_KEY:
                    unwrapped = HTMLElement(self, value[key])
                    break
                elif key == WEBELEMENT_KEY:
                    unwrapped = HTMLElement(self, value[key])
                    break
                else:
                    unwrapped[key] = self._from_json(value[key])
        else:
            unwrapped = value
        return unwrapped

    def execute_script(self, script, script_args=(), new_sandbox=True,
                       sandbox="default", script_timeout=None):
        """Executes a synchronous JavaScript script, and returns the
        result (or None if the script does return a value).

        The script is executed in the context set by the most recent
        :func:`set_context` call, or to the CONTEXT_CONTENT context if
        :func:`set_context` has not been called.

        :param script: A string containing the JavaScript to execute.
        :param script_args: An interable of arguments to pass to the script.
        :param sandbox: A tag referring to the sandbox you wish to use;
            if you specify a new tag, a new sandbox will be created.
            If you use the special tag `system`, the sandbox will
            be created using the system principal which has elevated
            privileges.
        :param new_sandbox: If False, preserve global variables from
            the last execute_*script call. This is True by default, in which
            case no globals are preserved.

        Simple usage example:

        ::

            result = marionette.execute_script("return 1;")
            assert result == 1

        You can use the `script_args` parameter to pass arguments to the
        script:

        ::

            result = marionette.execute_script("return arguments[0] + arguments[1];",
                                               script_args=(2, 3,))
            assert result == 5
            some_element = marionette.find_element(By.ID, "someElement")
            sid = marionette.execute_script("return arguments[0].id;", script_args=(some_element,))
            assert some_element.get_attribute("id") == sid

        Scripts wishing to access non-standard properties of the window
        object must use window.wrappedJSObject:

        ::

            result = marionette.execute_script('''
              window.wrappedJSObject.test1 = "foo";
              window.wrappedJSObject.test2 = "bar";
              return window.wrappedJSObject.test1 + window.wrappedJSObject.test2;
              ''')
            assert result == "foobar"

        Global variables set by individual scripts do not persist between
        script calls by default.  If you wish to persist data between
        script calls, you can set `new_sandbox` to False on your next call,
        and add any new variables to a new 'global' object like this:

        ::

            marionette.execute_script("global.test1 = 'foo';")
            result = self.marionette.execute_script("return global.test1;", new_sandbox=False)
            assert result == "foo"

        """
        args = self._to_json(script_args)
        stack = traceback.extract_stack()
        frame = stack[-2:-1][0]  # grab the second-to-last frame
        filename = frame[0] if sys.platform == "win32" else os.path.relpath(frame[0])
        body = {"script": script.strip(),
                "args": args,
                "newSandbox": new_sandbox,
                "sandbox": sandbox,
                "scriptTimeout": script_timeout,
                "line": int(frame[1]),
                "filename": filename}
        rv = self._send_message("WebDriver:ExecuteScript", body, key="value")
        return self._from_json(rv)

    def execute_async_script(self, script, script_args=(), new_sandbox=True,
                             sandbox="default", script_timeout=None):
        """Executes an asynchronous JavaScript script, and returns the
        result (or None if the script does return a value).

        The script is executed in the context set by the most recent
        :func:`set_context` call, or to the CONTEXT_CONTENT context if
        :func:`set_context` has not been called.

        :param script: A string containing the JavaScript to execute.
        :param script_args: An interable of arguments to pass to the script.
        :param sandbox: A tag referring to the sandbox you wish to use; if
            you specify a new tag, a new sandbox will be created.  If you
            use the special tag `system`, the sandbox will be created
            using the system principal which has elevated privileges.
        :param new_sandbox: If False, preserve global variables from
            the last execute_*script call. This is True by default,
            in which case no globals are preserved.

        Usage example:

        ::

            marionette.timeout.script = 10
            result = self.marionette.execute_async_script('''
              // this script waits 5 seconds, and then returns the number 1
              setTimeout(function() {
                marionetteScriptFinished(1);
              }, 5000);
            ''')
            assert result == 1
        """
        args = self._to_json(script_args)
        stack = traceback.extract_stack()
        frame = stack[-2:-1][0]  # grab the second-to-last frame
        filename = frame[0] if sys.platform == "win32" else os.path.relpath(frame[0])
        body = {"script": script.strip(),
                "args": args,
                "newSandbox": new_sandbox,
                "sandbox": sandbox,
                "scriptTimeout": script_timeout,
                "line": int(frame[1]),
                "filename": filename}

        rv = self._send_message("WebDriver:ExecuteAsyncScript", body, key="value")
        return self._from_json(rv)

    def find_element(self, method, target, id=None):
        """Returns an :class:`~marionette_driver.marionette.HTMLElement`
        instance that matches the specified method and target in the current
        context.

        An :class:`~marionette_driver.marionette.HTMLElement` instance may be
        used to call other methods on the element, such as
        :func:`~marionette_driver.marionette.HTMLElement.click`.  If no element
        is immediately found, the attempt to locate an element will be repeated
        for up to the amount of time set by
        :attr:`marionette_driver.timeout.Timeouts.implicit`. If multiple
        elements match the given criteria, only the first is returned. If no
        element matches, a ``NoSuchElementException`` will be raised.

        :param method: The method to use to locate the element; one of:
            "id", "name", "class name", "tag name", "css selector",
            "link text", "partial link text", "xpath", "anon" and "anon
            attribute". Note that the "name", "link text" and "partial
            link test" methods are not supported in the chrome DOM.
        :param target: The target of the search.  For example, if method =
            "tag", target might equal "div".  If method = "id", target would
            be an element id.
        :param id: If specified, search for elements only inside the element
            with the specified id.
        """
        body = {"value": target, "using": method}
        if id:
            body["element"] = id

        return self._send_message("WebDriver:FindElement",
                                  body, key="value")

    def find_elements(self, method, target, id=None):
        """Returns a list of all
        :class:`~marionette_driver.marionette.HTMLElement` instances that match
        the specified method and target in the current context.

        An :class:`~marionette_driver.marionette.HTMLElement` instance may be
        used to call other methods on the element, such as
        :func:`~marionette_driver.marionette.HTMLElement.click`.  If no element
        is immediately found, the attempt to locate an element will be repeated
        for up to the amount of time set by
        :attr:`marionette_driver.timeout.Timeouts.implicit`.

        :param method: The method to use to locate the elements; one
            of: "id", "name", "class name", "tag name", "css selector",
            "link text", "partial link text", "xpath", "anon" and "anon
            attribute". Note that the "name", "link text" and "partial link
            test" methods are not supported in the chrome DOM.
        :param target: The target of the search.  For example, if method =
            "tag", target might equal "div".  If method = "id", target would be
            an element id.
        :param id: If specified, search for elements only inside the element
            with the specified id.
        """
        body = {"value": target, "using": method}
        if id:
            body["element"] = id

        return self._send_message("WebDriver:FindElements",
                                  body)

    def get_active_element(self):
        el_or_ref = self._send_message("WebDriver:GetActiveElement",
                                       key="value")
        return el_or_ref

    def add_cookie(self, cookie):
        """Adds a cookie to your current session.

        :param cookie: A dictionary object, with required keys - "name"
            and "value"; optional keys - "path", "domain", "secure",
            "expiry".

        Usage example:

        ::

            driver.add_cookie({"name": "foo", "value": "bar"})
            driver.add_cookie({"name": "foo", "value": "bar", "path": "/"})
            driver.add_cookie({"name": "foo", "value": "bar", "path": "/",
                               "secure": True})
        """
        self._send_message("WebDriver:AddCookie",
                           {"cookie": cookie})

    def delete_all_cookies(self):
        """Delete all cookies in the scope of the current session.

        Usage example:

        ::

            driver.delete_all_cookies()
        """
        self._send_message("WebDriver:DeleteAllCookies")

    def delete_cookie(self, name):
        """Delete a cookie by its name.

        :param name: Name of cookie to delete.

        Usage example:

        ::

            driver.delete_cookie("foo")
        """
        self._send_message("WebDriver:DeleteCookie",
                           {"name": name})

    def get_cookie(self, name):
        """Get a single cookie by name. Returns the cookie if found,
        None if not.

        :param name: Name of cookie to get.
        """
        cookies = self.get_cookies()
        for cookie in cookies:
            if cookie["name"] == name:
                return cookie
        return None

    def get_cookies(self):
        """Get all the cookies for the current domain.

        This is the equivalent of calling `document.cookie` and
        parsing the result.

        :returns: A list of cookies for the current domain.
        """
        return self._send_message("WebDriver:GetCookies")

    def screenshot(self, element=None, highlights=None, format="base64",
                   full=True, scroll=True):
        """Takes a screenshot of a web element or the current frame.

        The screen capture is returned as a lossless PNG image encoded
        as a base 64 string by default. If the `element` argument is defined the
        capture area will be limited to the bounding box of that
        element.  Otherwise, the capture area will be the bounding box
        of the current frame.

        :param element: The element to take a screenshot of.  If None, will
            take a screenshot of the current frame.

        :param highlights: A list of
            :class:`~marionette_driver.marionette.HTMLElement` objects to draw
            a red box around in the returned screenshot.

        :param format: if "base64" (the default), returns the screenshot
            as a base64-string. If "binary", the data is decoded and
            returned as raw binary. If "hash", the data is hashed using
            the SHA-256 algorithm and the result is returned as a hex digest.

        :param full: If True (the default), the capture area will be the
            complete frame. Else only the viewport is captured. Only applies
            when `element` is None.

        :param scroll: When `element` is provided, scroll to it before
            taking the screenshot (default).  Otherwise, avoid scrolling
            `element` into view.
        """

        if element:
            element = element.id
        lights = None
        if highlights:
            lights = [highlight.id for highlight in highlights]

        body = {"id": element,
                "highlights": lights,
                "full": full,
                "hash": False,
                "scroll": scroll}
        if format == "hash":
            body["hash"] = True

        data = self._send_message("WebDriver:TakeScreenshot",
                                  body, key="value")

        if format == "base64" or format == "hash":
            return data
        elif format == "binary":
            return base64.b64decode(data.encode("ascii"))
        else:
            raise ValueError("format parameter must be either 'base64'"
                             " or 'binary', not {0}".format(repr(format)))

    @property
    def orientation(self):
        """Get the current browser orientation.

        Will return one of the valid primary orientation values
        portrait-primary, landscape-primary, portrait-secondary, or
        landscape-secondary.
        """
        try:
            return self._send_message("Marionette:GetScreenOrientation",
                                      key="value")
        except errors.UnknownCommandException:
            return self._send_message("getScreenOrientation", key="value")

    def set_orientation(self, orientation):
        """Set the current browser orientation.

        The supplied orientation should be given as one of the valid
        orientation values.  If the orientation is unknown, an error
        will be raised.

        Valid orientations are "portrait" and "landscape", which fall
        back to "portrait-primary" and "landscape-primary"
        respectively, and "portrait-secondary" as well as
        "landscape-secondary".

        :param orientation: The orientation to lock the screen in.
        """
        body = {"orientation": orientation}
        try:
            self._send_message("Marionette:SetScreenOrientation", body)
        except errors.UnknownCommandException:
            self._send_message("setScreenOrientation", body)

    @property
    def window_size(self):
        """Get the current browser window size.

        Will return the current browser window size in pixels. Refers to
        window outerWidth and outerHeight values, which include scroll bars,
        title bars, etc.

        :returns: Window rect.
        """
        warnings.warn("window_size property has been deprecated, please use get_window_rect()",
                      DeprecationWarning)
        return self._send_message("getWindowSize")

    def set_window_size(self, width, height):
        """Resize the browser window currently in focus.

        The supplied ``width`` and ``height`` values refer to the window `outerWidth`
        and `outerHeight` values, which include scroll bars, title bars, etc.

        An error will be returned if the requested window size would result
        in the window being in the maximised state.

        :param width: The width to resize the window to.
        :param height: The height to resize the window to.

        :returns Window rect.
        """
        warnings.warn("set_window_size() has been deprecated, please use set_window_rect()",
                      DeprecationWarning)
        body = {"width": width, "height": height}
        return self._send_message("setWindowSize", body)

    def minimize_window(self):
        """Iconify the browser window currently receiving commands.
        The action should be equivalent to the user pressing the minimize
        button in the OS window.

        Note that this command is not available on Fennec.  It may also
        not be available in certain window managers.

        :returns Window rect.
        """
        return self._send_message("WebDriver:MinimizeWindow")

    def maximize_window(self):
        """Resize the browser window currently receiving commands.
        The action should be equivalent to the user pressing the maximize
        button in the OS window.


        Note that this command is not available on Fennec.  It may also
        not be available in certain window managers.

        :returns: Window rect.
        """
        return self._send_message("WebDriver:MaximizeWindow")

    def fullscreen(self):
        """Synchronously sets the user agent window to full screen as
        if the user had done "View > Enter Full Screen",  or restores
        it if it is already in full screen.

        :returns: Window rect.
        """
        return self._send_message("WebDriver:FullscreenWindow")
