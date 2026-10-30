package com.cocos.lib;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Backs jsb.selectFiles: opens the system document picker and copies the picked
 * documents into the app cache, because native file APIs cannot read content:// URIs.
 */
public class CocosFilePicker {
    static final int REQUEST_CODE = 0x4346;
    private static final String CACHE_DIR = "selected-files";

    // Called from the game thread through JNI.
    public static void select(final String extension) {
        GlobalObject.runOnUiThread(() -> {
            Activity activity = GlobalObject.getActivity();
            if (activity == null) {
                finish(new String[0], "File selection has no active window");
                return;
            }
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            intent.setType("*/*");
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
            // Some providers report downloaded archives as octet-stream; the caller validates contents.
            if (mime != null) intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{mime, "application/octet-stream"});
            try {
                activity.startActivityForResult(intent, REQUEST_CODE);
            } catch (ActivityNotFoundException e) {
                finish(new String[0], "No file picker is available");
            }
        });
    }

    /** Returns true when the result belongs to a file selection. */
    static boolean onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_CODE) return false;
        final List<Uri> uris = new ArrayList<>();
        if (resultCode == Activity.RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }
        final ContentResolver resolver = activity.getContentResolver();
        final File root = new File(activity.getCacheDir(), CACHE_DIR);
        // Archives can be large, so copy off the UI thread.
        new Thread(() -> copy(resolver, uris, root), "CocosFilePicker").start();
        return true;
    }

    private static void copy(ContentResolver resolver, List<Uri> uris, File root) {
        // Only one selection is open at a time and earlier copies are already consumed.
        deleteRecursively(root);
        String[] paths = new String[uris.size()];
        byte[] buffer = new byte[64 * 1024];
        for (int i = 0; i < uris.size(); i++) {
            Uri uri = uris.get(i);
            File dir = new File(root, String.valueOf(i));
            File file = new File(dir, displayName(resolver, uri, i));
            try {
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Could not create " + dir);
                try (InputStream in = resolver.openInputStream(uri); OutputStream out = new FileOutputStream(file)) {
                    if (in == null) throw new IOException("Could not open " + uri);
                    int read;
                    while ((read = in.read(buffer)) != -1) out.write(buffer, 0, read);
                }
            } catch (IOException | SecurityException e) {
                deleteRecursively(root);
                finish(new String[0], "Could not read " + file.getName());
                return;
            }
            paths[i] = file.getAbsolutePath();
        }
        finish(paths, null);
    }

    private static String displayName(ContentResolver resolver, Uri uri, int index) {
        String name = null;
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
        } catch (RuntimeException ignored) {
        }
        if (name == null || name.isEmpty()) name = uri.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "file-" + index;
        return name.replace('/', '_');
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        file.delete();
    }

    private static void finish(final String[] paths, final String error) {
        CocosHelper.runOnGameThread(() -> onFilesSelectedNative(paths, error));
    }

    private static native void onFilesSelectedNative(String[] paths, String error);
}
