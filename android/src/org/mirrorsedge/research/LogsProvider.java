package org.mirrorsedge.research;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

/**
 * Read-only access to the log zips in cache/logs for the share sheet (the receiving app gets
 * a one-time read grant through the share intent). Nothing else is reachable: the name must
 * be a plain file name inside that directory.
 */
public class LogsProvider extends ContentProvider {
    static final String AUTHORITY = "org.mirrorsedge.research.logs";

    private File file(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || name.contains("..") || !name.endsWith(".zip")) throw new FileNotFoundException();
        File f = new File(new File(getContext().getCacheDir(), "logs"), name);
        if (!f.isFile()) throw new FileNotFoundException(name);
        return f;
    }

    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return "application/zip"; }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            File f = file(uri);
            MatrixCursor cursor = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            cursor.addRow(new Object[] {f.getName(), f.length()});
            return cursor;
        } catch (FileNotFoundException e) {
            return null;
        }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
