import os

BRANCH = "jamun"
MOZILLA_DIR = BRANCH
EN_US_BINARY_URL = None     # No build has been uploaded to archive.m.o

config = {
    "branch": BRANCH,
    "log_name": "single_locale",
    "objdir": "obj-firefox",
    "is_automation": True,
    "buildbot_json_path": "buildprops.json",
    "force_clobber": True,
    "clobberer_url": "https://api.pub.build.mozilla.org/clobberer/lastclobber",
    "locales_file": "%s/mobile/locales/l10n-changesets.json" % MOZILLA_DIR,
    "locales_dir": "mobile/android/locales",
    "ignore_locales": ["en-US"],
    "nightly_build": True,
    'balrog_credentials_file': 'oauth.txt',
    "tools_repo": "https://hg.mozilla.org/build/tools",
    "tooltool_config": {
        "manifest": "mobile/android/config/tooltool-manifests/android/releng.manifest",
        "output_dir": "%(abs_work_dir)s/" + MOZILLA_DIR,
    },
    "repos": [{
        "vcs": "hg",
        "repo": "https://hg.mozilla.org/build/tools",
        "branch": "default",
        "dest": "tools",
    }, {
        "vcs": "hg",
        "repo": "https://hg.mozilla.org/projects/jamun",
        "revision": "%(revision)s",
        "dest": MOZILLA_DIR,
    }],
    "hg_l10n_base": "https://hg.mozilla.org/l10n-central",
    "hg_l10n_tag": "default",
    'vcs_share_base': "/builds/hg-shared",

    "l10n_dir": "mozilla-beta",
    "repack_env": {
        # so ugly, bug 951238
        "LD_LIBRARY_PATH": "/lib:/tools/gcc-4.7.2-0moz1/lib:/tools/gcc-4.7.2-0moz1/lib64",
        "MOZ_OBJDIR": "obj-firefox",
        "EN_US_BINARY_URL": os.environ.get("EN_US_BINARY_URL", EN_US_BINARY_URL),
        "MOZ_UPDATE_CHANNEL": "nightly-jamun",
    },
    "upload_branch": "%s-android-api-16" % BRANCH,
    "signature_verification_script": "tools/release/signing/verify-android-signature.sh",
    "platform": "android",

    # Balrog
    "build_target": "Android_arm-eabi-gcc3",

    # Mock
    "mock_target": "mozilla-centos6-x86_64-android",
    "mock_packages": ['autoconf213', 'python', 'zip', 'mozilla-python27-mercurial', 'git', 'ccache',
                      'glibc-static', 'libstdc++-static', 'perl-Test-Simple', 'perl-Config-General',
                      'gtk2-devel', 'libnotify-devel', 'yasm',
                      'alsa-lib-devel', 'libcurl-devel',
                      'wireless-tools-devel', 'libX11-devel',
                      'libXt-devel', 'mesa-libGL-devel',
                      'gnome-vfs2-devel', 'GConf2-devel', 'wget',
                      'mpfr',  # required for system compiler
                      'xorg-x11-font*',  # fonts required for PGO
                      'imake',  # required for makedepend!?!
                      'gcc45_0moz3', 'gcc454_0moz1', 'gcc472_0moz1', 'gcc473_0moz1', 'yasm', 'ccache',  # <-- from releng repo
                      'valgrind', 'dbus-x11',
                      'pulseaudio-libs-devel',
                      'gstreamer-devel', 'gstreamer-plugins-base-devel',
                      'freetype-2.3.11-6.el6_1.8.x86_64',
                      'freetype-devel-2.3.11-6.el6_1.8.x86_64',
                      'java-1.7.0-openjdk-devel',
                      'openssh-clients',
                      'zlib-devel-1.2.3-27.el6.i686',
                      ],
    "mock_files": [
        ("/home/cltbld/.ssh", "/home/mock_mozilla/.ssh"),
        ('/home/cltbld/.hgrc', '/builds/.hgrc'),
        ('/builds/relengapi.tok', '/builds/relengapi.tok'),
        ('/usr/local/lib/hgext', '/usr/local/lib/hgext'),
    ],
}
