Preferences
===========

Telemetry behaviour is controlled through the mozconfig defines and preferences listed here.

mozconfig Defines
-----------------

``MOZ_TELEMETRY_REPORTING``

  When Defined (which it is for official builds):

  * If ``RELEASE_OR_BETA`` is not defined, defines ``MOZ_TELEMETRY_ON_BY_DEFAULT``

  When Not Defined:

  * If ``datareporting.healthreport.uploadEnabled`` is locked, we print a message in the Privacy settings that you cannot turn on data submission and disabled the checkbox so you don't try.
  * Android: hides the data submission UI to prevent users from thinking they can turn it on
  * Disables Telemetry from being sent (due to ``Telemetry::IsOfficialTelemetry``)

``MOZ_TELEMETRY_ON_BY_DEFAULT``

  When Defined:

  * Android: enables ``toolkit.telemetry.enabled``

``MOZ_SERVICES_HEALTHREPORT``

  When Defined (which it is on most platforms):

  * includes ``toolkit/components/telemetry/healthreport-prefs.js`` (which sets ``datareporting.healthreport.{infoURL|uploadEnabled}``)

``MOZ_DATA_REPORTING``

  When Defined (which it is when ``MOZ_TELEMETRY_REPORTING``, ``MOZ_SERVICES_HEALTHREPORT``, or ``MOZ_CRASHREPORTER`` is defined (so, on most platforms, but not typically on developer builds)):

  * Enables ``app.shield.optoutstudies.enabled``

  When Not Defined:

  * Disables ``app.shield.optoutstudies.enabled``
  * Removes the Data Collection Preferences UI in ``privacy.xul``

``MOZILLA_OFFICIAL``

  When Not Defined (defined on our own external builds and builds from several Linux distros, but not typically on defeloper builds):

  * Disables Telemetry from being sent (due to ``Telemetry::IsOfficialTelemetry``)

``MOZ_UPDATE_CHANNEL``

  When not ``release`` or ``beta``:

  * If ``MOZ_TELEMETRY_REPORTING`` is also defined, defines ``MOZ_TELEMETRY_ON_BY_DEFAULT``

  When ``beta``:

  * If ``toolkit.telemetry.enabled`` is otherwise unset at startup, ``toolkit.telemetry.enabled`` is defaulted to ``true`` (this is irrespective of ``MOZ_TELEMETRY_REPORTING``)

  When ``nightly`` or ``aurora`` or ``beta`` or ``default``:

  * Desktop: Locks ``toolkit.telemetry.enabled`` to ``true``. All other values for ``MOZ_UPDATE_CHANNEL`` on Desktop locks ``toolkit.telemetry.enabled`` to ``false``.
  * Desktop: Defaults ``Telemetry::CanRecordExtended`` (and, thus ``Telemetry::CanRecordReleaseData``) to ``true``. All other values of ``MOZ_UPDATE_CHANNEL`` on Desktop defaults these to ``false``.

``DEBUG``

  When Defined:

  * Disables Telemetry from being sent (due to ``Telemetry::IsOfficialTelemetry``)

**In Short:**

  For builds downloaded from mozilla.com ``MOZ_TELEMETRY_REPORTING`` is defined, ``MOZ_TELEMETRY_ON_BY_DEFAULT`` is on if you downloaded Nightly or Developer Edition, ``MOZ_SERVICES_HEALTHREPORT`` is defined, ``MOZ_DATA_REPORTING`` is defined, ``MOZILLA_OFFICIAL`` is defined, ``MOZ_UPDATE_CHANNEL`` is set to the channel you downloaded, and ``DEBUG`` is false. This means Telemetry is, by default, collecting some amount of information and is sending it to Mozilla.

  For builds you make yourself with a blank mozconfig, ``MOZ_UPDATE_CHANNEL`` is set to ``default`` and everything else is undefined. This means Telemetry is, by default, collecting an extended amount of information but isn't sending it anywhere.

Preferences
-----------

``toolkit.telemetry.unified``

  This controls whether unified behavior is enabled. If true:

  * Telemetry is always enabled and recording *base* data.
  * Telemetry will send additional ``main`` pings.

  It defaults to ``true``, but is ``false`` on Android (Fennec) builds.

``toolkit.telemetry.enabled``

  If ``unified`` is off, this controls whether the Telemetry module is enabled. It can be set or unset via the `Preferences` dialog in Firefox for Android (Fennec).
  If ``unified`` is on, this is locked to ``true`` if ``MOZ_UPDATE_CHANNEL`` is ``nightly`` or ``aurora`` or ``beta`` or ``default`` (which is the default value of ``MOZ_UPDATE_CHANNEL`` for developer builds). Otherwise it is locked to ``false``. This controls a diminishing number of things and is intended to be deprecated, and then removed.

``datareporting.healthreport.uploadEnabled``

  If ``unified`` is true, this controls whether we send Telemetry data.
  If ``unified`` is false, we don't use this value.

``toolkit.telemetry.archive.enabled``

  Allow pings to be archived locally. This can only be enabled if ``unified`` is on.

``toolkit.telemetry.server``

  The server Telemetry pings are sent to.

``toolkit.telemetry.log.level``

  This sets the Telemetry logging verbosity per ``Log.jsm``, with ``Trace`` or ``0`` being the most verbose and the default being ``Warn``.
  By default logging goes only the console service.

``toolkit.telemetry.log.dump``

  Sets whether to dump Telemetry log messages to ``stdout`` too.

``toolkit.telemetry.shutdownPingSender.enabled``

  Allow the ``shutdown`` ping to be sent when the browser shuts down, from the second browsing session on, instead of the next restart, using the :doc:`ping sender <pingsender>`.

``toolkit.telemetry.shutdownPingSender.enabledFirstSession``

  Allow the ``shutdown`` ping to be sent using the :doc:`ping sender <pingsender>` from the first browsing session.

``toolkit.telemetry.firstShutdownPing.enabled``

  Allow a duplicate of the ``main`` shutdown ping from the first browsing session to be sent as a separate ``first-shutdown`` ping.

``toolkit.telemetry.newProfilePing.enabled``

  Enable the :doc:`../data/new-profile-ping` on new profiles.

``toolkit.telemetry.newProfilePing.delay``

  Controls the delay after which the :doc:`../data/new-profile-ping` is sent on new profiles.

``toolkit.telemetry.updatePing.enabled``

  Enable the :doc:`../data/update-ping` on browser updates.

``toolkit.telemetry.maxEventSummaryKeys``

  Set the maximum number of keys per process of the :ref:`Event Summary <events.event-summary>`
  :ref:`keyed scalars <scalars.keyed-scalars>`. Default is 500. Change requires restart.

Data-choices notification
-------------------------

``toolkit.telemetry.reportingpolicy.firstRun``

  This preference is not present until the first run. After, its value is set to false. This is used to show the infobar with a more aggressive timeout if it wasn't shown yet.

``datareporting.policy.firstRunURL``

  If set, a browser tab will be opened on first run instead of the infobar.

``datareporting.policy.dataSubmissionEnabled``

  This is the data submission master kill switch. If disabled, no policy is shown or upload takes place, ever.

``datareporting.policy.dataSubmissionPolicyNotifiedTime``

  Records the date user was shown the policy. This preference is also used on Android.

``datareporting.policy.dataSubmissionPolicyAcceptedVersion``

  Records the version of the policy notified to the user. This preference is also used on Android.

``datareporting.policy.dataSubmissionPolicyBypassNotification``

  Used in tests, it allows to skip the notification check.

``datareporting.policy.currentPolicyVersion``

  Stores the current policy version, overrides the default value defined in TelemetryReportingPolicy.jsm.

``datareporting.policy.minimumPolicyVersion``

  The minimum policy version that is accepted for the current policy. This can be set per channel.

``datareporting.policy.minimumPolicyVersion.channel-NAME``

  This is the only channel-specific version that we currently use for the minimum policy version.

Testing
-------

The following prefs are for testing purpose only.

``toolkit.telemetry.initDelay``

  Delay before initializing telemetry (seconds).

``toolkit.telemetry.minSubsessionLength``

  Minimum length of a telemetry subsession and throttling time for common environment changes (seconds).

``toolkit.telemetry.collectInterval``

  Minimum interval between data collection (seconds).

``toolkit.telemetry.scheduler.tickInterval``

  Interval between scheduler ticks (seconds).

``toolkit.telemetry.scheduler.idleTickInterval``

  Interval between scheduler ticks when the user is idle (seconds).

``toolkit.telemetry.idleTimeout``

  Timeout until we decide whether a user is idle or not (seconds).

``toolkit.telemetry.modulesPing.interval``

  Interval between "modules" ping transmissions.

``toolkit.telemetry.send.overrideOfficialCheck``

  If true, allows sending pings on unofficial builds. Requires a restart.

``toolkit.telemetry.testing.overridePreRelease``

  If true, allows recording opt-in Telemetry on the Release channel. Requires a restart.

``toolkit.telemetry.healthping.enabled``

  If false, sending health pings is disabled. Defaults to true.
