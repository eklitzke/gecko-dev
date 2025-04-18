/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko;

import org.mozilla.gecko.permissions.PermissionBlock;
import org.mozilla.gecko.permissions.Permissions;
import org.mozilla.gecko.util.BundleEventListener;
import org.mozilla.gecko.util.EventCallback;
import org.mozilla.gecko.util.GeckoBundle;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.Environment;
import android.os.Parcelable;
import android.support.annotation.NonNull;
import android.text.TextUtils;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;

public class FilePicker implements BundleEventListener {
    private static final String LOGTAG = "GeckoFilePicker";
    private static FilePicker sFilePicker;
    private static final int MODE_OPEN_MULTIPLE_ATTRIBUTE_VALUE = 3;
    private static final int MODE_OPEN_SINGLE_ATTRIBUTE_VALUE = 0;
    private final Context context;

    public interface ResultHandler {
        void gotFile(String filename);
    }

    public static void init(Context context) {
        if (sFilePicker == null) {
            sFilePicker = new FilePicker(context.getApplicationContext());
        }
    }

    private FilePicker(Context context) {
        this.context = context;
        EventDispatcher.getInstance().registerUiThreadListener(this, "FilePicker:Show");
    }

    @Override // BundleEventListener
    public void handleMessage(final String event, final GeckoBundle message,
                              final EventCallback callback) {
        if ("FilePicker:Show".equals(event)) {
            String mimeType = "*/*";
            final String mode = message.getString("mode", "");
            final int tabId = message.getInt("tabId", -1);
            final String title = message.getString("title", "");
            final boolean isModeOpenMultiple = message.getInt("modeOpenAttribute", MODE_OPEN_SINGLE_ATTRIBUTE_VALUE)
                    == MODE_OPEN_MULTIPLE_ATTRIBUTE_VALUE;

            if ("mimeType".equals(mode)) {
                mimeType = message.getString("mimeType", "");
            } else if ("extension".equals(mode)) {
                mimeType = GeckoAppShell.getMimeTypeFromExtensions(message.getString("extensions", ""));
            }

            final String[] requiredPermission = getPermissionsForMimeType(mimeType);
            final String finalMimeType = mimeType;

            // Use activity context because we want to prompt for runtime permission.
            final Activity currentActivity =
                    GeckoActivityMonitor.getInstance().getCurrentActivity();
            final PermissionBlock perm;
            if (currentActivity != null) {
                perm = Permissions.from(currentActivity);
            } else {
                perm = Permissions.from(context).doNotPrompt();
            }

            perm.withPermissions(requiredPermission)
                .andFallback(new Runnable() {
                    @Override
                    public void run() {
                        // In the fallback case, we still show the picker, just
                        // with the default file list.
                        // TODO: Figure out which permissions have been denied and use that
                        // knowledge for availPermissions. For now we assume we don't have any
                        // permissions at all (bug 1411014).
                        showFilePickerAsync(title, "*/*", new String[0], isModeOpenMultiple, new ResultHandler() {
                            @Override
                            public void gotFile(final String filename) {
                                callback.sendSuccess(filename);
                            }
                        }, tabId);
                    }
                })
                .run(new Runnable() {
                    @Override
                    public void run() {
                        showFilePickerAsync(title, finalMimeType, requiredPermission, isModeOpenMultiple, new ResultHandler() {
                            @Override
                            public void gotFile(final String filename) {
                                callback.sendSuccess(filename);
                            }
                        }, tabId);
                    }
                });
        }
    }

    private static String[] getPermissionsForMimeType(final String mimeType) {
        if (mimeType.startsWith("audio/")) {
            return new String[] { Manifest.permission.READ_EXTERNAL_STORAGE };
        } else if (mimeType.startsWith("image/")) {
            return new String[] { Manifest.permission.CAMERA, Manifest.permission.READ_EXTERNAL_STORAGE };
        } else if (mimeType.startsWith("video/")) {
            return new String[] { Manifest.permission.CAMERA, Manifest.permission.READ_EXTERNAL_STORAGE };
        }
        return new String[] { Manifest.permission.CAMERA, Manifest.permission.READ_EXTERNAL_STORAGE };
    }

    private static boolean hasPermissionsForMimeType(final String mimeType, final String[] availPermissions) {
        return Arrays.asList(availPermissions)
                .containsAll(Arrays.asList(getPermissionsForMimeType(mimeType)));
    }

    private void addActivities(Intent intent, HashMap<String, Intent> intents, HashMap<String, Intent> filters) {
        PackageManager pm = context.getPackageManager();
        List<ResolveInfo> lri = pm.queryIntentActivities(intent, 0);
        for (ResolveInfo ri : lri) {
            ComponentName cn = new ComponentName(ri.activityInfo.applicationInfo.packageName, ri.activityInfo.name);
            if (filters != null && !filters.containsKey(cn.toString())) {
                Intent rintent = new Intent(intent);
                rintent.setComponent(cn);
                intents.put(cn.toString(), rintent);
            }
        }
    }

    private Intent getIntent(String mimeType) {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType(mimeType);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        return intent;
    }

    private List<Intent> getIntentsForFilePicker(final @NonNull String mimeType,
                                                 final @NonNull String[] availPermissions,
                                                 final boolean isModeOpenMultiple,
                                                 final FilePickerResultHandler fileHandler) {
        // The base intent to use for the file picker. Even if this is an implicit intent, Android will
        // still show a list of Activities that match this action/type.
        Intent baseIntent;
        // A HashMap of Activities the base intent will show in the chooser. This is used
        // to filter activities from other intents so that we don't show duplicates.
        HashMap<String, Intent> baseIntents = new HashMap<String, Intent>();
        // A list of other activities to show in the picker (and the intents to launch them).
        HashMap<String, Intent> intents = new HashMap<String, Intent> ();

        baseIntent = getIntent(mimeType);
        if (isModeOpenMultiple) {
            baseIntent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }

        if (mimeType.startsWith("audio/")) {
            addActivities(baseIntent, baseIntents, null);
            if (mimeType.equals("audio/*") &&
                    hasPermissionsForMimeType(mimeType, availPermissions)) {
                // We also add a capture intent
                Intent intent = IntentHelper.getAudioCaptureIntent();
                addActivities(intent, intents, baseIntents);
            }
        } else if (mimeType.startsWith("image/") ) {
            addActivities(baseIntent, baseIntents, null);
            if (mimeType.equals("image/*") &&
                    hasPermissionsForMimeType(mimeType, availPermissions)) {
                // We also add a capture intent
                Intent intent = IntentHelper.getImageCaptureIntent(
                        new File(Environment.getExternalStorageDirectory(),
                                fileHandler.generateImageName()));
                addActivities(intent, intents, baseIntents);
            }
        } else if (mimeType.startsWith("video/")) {
            addActivities(baseIntent, baseIntents, null);
            if (mimeType.equals("video/*") &&
                    hasPermissionsForMimeType(mimeType, availPermissions)) {
                // We also add a capture intent
                Intent intent = IntentHelper.getVideoCaptureIntent();
                addActivities(intent, intents, baseIntents);
            }
        } else {
            baseIntent = getIntent("*/*");
            if (isModeOpenMultiple) {
                baseIntent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            }
            addActivities(baseIntent, baseIntents, null);

            Intent intent;
            if (hasPermissionsForMimeType("audio/*", availPermissions)) {
                intent = IntentHelper.getAudioCaptureIntent();
                addActivities(intent, intents, baseIntents);
            }
            if (hasPermissionsForMimeType("image/*", availPermissions)) {
                intent = IntentHelper.getImageCaptureIntent(
                        new File(Environment.getExternalStorageDirectory(),
                                fileHandler.generateImageName()));
                addActivities(intent, intents, baseIntents);
            }
            if (hasPermissionsForMimeType("video/*", availPermissions)) {
                intent = IntentHelper.getVideoCaptureIntent();
                addActivities(intent, intents, baseIntents);
            }
        }

        // If we didn't find any activities, we fall back to the */* mimetype intent
        if (baseIntents.size() == 0 && intents.size() == 0) {
            intents.clear();

            baseIntent = getIntent("*/*");
            addActivities(baseIntent, baseIntents, null);
        }

        ArrayList<Intent> vals = new ArrayList<Intent>(intents.values());
        vals.add(0, baseIntent);
        return vals;
    }

    private String getFilePickerTitle(String mimeType) {
        if (mimeType.equals("audio/*")) {
            return context.getString(R.string.filepicker_audio_title);
        } else if (mimeType.equals("image/*")) {
            return context.getString(R.string.filepicker_image_title);
        } else if (mimeType.equals("video/*")) {
            return context.getString(R.string.filepicker_video_title);
        } else {
            return context.getString(R.string.filepicker_title);
        }
    }

    /* Gets an intent that can open a particular mimetype. Will show a prompt with a list
     * of Activities that can handle the mietype. Asynchronously calls the handler when
     * one of the intents is selected. If the caller passes in null for the handler, will still
     * prompt for the activity, but will throw away the result.
     */
    private Intent getFilePickerIntent(String title,
                                       final @NonNull String mimeType,
                                       final @NonNull String[] availPermissions,
                                       final boolean isModeOpenMultiple,
                                       final FilePickerResultHandler fileHandler) {
        final List<Intent> intents = getIntentsForFilePicker(mimeType, availPermissions, isModeOpenMultiple, fileHandler);

        if (intents.size() == 0) {
            Log.i(LOGTAG, "no activities for the file picker!");
            return null;
        }

        final Intent base = intents.remove(0);

        if (intents.size() == 0) {
            return base;
        }

        if (TextUtils.isEmpty(title)) {
            title = getFilePickerTitle(mimeType);
        }
        final Intent chooser = Intent.createChooser(base, title);
        chooser.putExtra(Intent.EXTRA_INITIAL_INTENTS,
                         intents.toArray(new Parcelable[intents.size()]));
        return chooser;
    }

    /* Allows the user to pick an activity to load files from using a list prompt. Then opens the activity and
     * sends the file returned to the passed in handler. If a null handler is passed in, will still
     * pick and launch the file picker, but will throw away the result.
     */
    protected void showFilePickerAsync(final String title, final @NonNull String mimeType,
                                       final @NonNull String[] availPermissions,
                                       final boolean isModeOpenMultiple,
                                       final ResultHandler handler, final int tabId) {
        final FilePickerResultHandler fileHandler =
                new FilePickerResultHandler(handler, context, tabId);
        final Intent intent = getFilePickerIntent(title, mimeType, availPermissions, isModeOpenMultiple, fileHandler);
        final Activity currentActivity =
                GeckoActivityMonitor.getInstance().getCurrentActivity();

        if (intent == null || currentActivity == null) {
            handler.gotFile("");
            return;
        }

        ActivityHandlerHelper.startIntentForActivity(currentActivity, intent, fileHandler);
    }
}
