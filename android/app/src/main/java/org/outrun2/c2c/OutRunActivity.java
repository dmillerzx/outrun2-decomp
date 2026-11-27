package org.outrun2.c2c;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;

public class OutRunActivity extends SDLActivity {
    private static final int REQ_PICK_GAME_DIR = 4101;
    private static final int REQ_ALL_FILES_ACCESS = 4102;

    private static volatile OutRunActivity sInstance;
    private final Object mPromptLock = new Object();
    private volatile String mPromptResult = null;
    private volatile boolean mAwaitingSettingsReturn = false;
    private volatile String mLastTitle = null;
    private volatile String mLastMessage = null;
    private volatile boolean mLastAllowGrant = false;
    private AlertDialog mActiveDialog = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Ensure app-private external files dir exists for logs and fallback storage
        File extDir = getExternalFilesDir(null);
        if (extDir != null && !extDir.exists()) {
            extDir.mkdirs();
        }

        // Disable autofill to prevent Android 13 MemoryIntArray parceling crash
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            getWindow().getDecorView().setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        }

        applyImmersiveMode();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mAwaitingSettingsReturn) {
            mAwaitingSettingsReturn = false;
            finishPrompt("RETRY");
        }
    }

    @Override
    protected void onDestroy() {
        finishPrompt("EXIT");
        if (mActiveDialog != null && mActiveDialog.isShowing()) {
            try {
                mActiveDialog.dismiss();
            } catch (Exception ignored) {
            }
            mActiveDialog = null;
        }
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    private void finishPrompt(String result) {
        synchronized (mPromptLock) {
            if (mPromptResult == null) {
                mPromptResult = result;
                mPromptLock.notifyAll();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ALL_FILES_ACCESS) {
            mAwaitingSettingsReturn = false;
            finishPrompt("RETRY");
            return;
        }
        if (requestCode == REQ_PICK_GAME_DIR) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                Uri uri = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {
                }
                String resolved = resolveTreeUriPath(uri);
                if (resolved != null && !resolved.isEmpty()) {
                    writeGamePathFile(resolved);
                }
                finishPrompt("RETRY");
            } else if (mLastTitle != null && mLastMessage != null && !isFinishing()) {
                showDialogOnUiThread(mLastTitle, mLastMessage, mLastAllowGrant);
            } else {
                finishPrompt("RETRY");
            }
        }
    }

    private String resolveTreeUriPath(Uri uri) {
        try {
            String docId = DocumentsContract.getTreeDocumentId(uri);
            if (docId == null || docId.isEmpty()) {
                return null;
            }
            int colon = docId.indexOf(':');
            String volume = (colon >= 0) ? docId.substring(0, colon) : docId;
            String rel = (colon >= 0) ? docId.substring(colon + 1) : "";
            String base;
            if ("primary".equalsIgnoreCase(volume)) {
                base = Environment.getExternalStorageDirectory().getAbsolutePath();
            } else if (!volume.isEmpty()) {
                base = "/storage/" + volume;
            } else {
                return null;
            }
            while (rel.startsWith("/")) {
                rel = rel.substring(1);
            }
            return rel.isEmpty() ? base : (base + "/" + rel);
        } catch (Exception e) {
            return null;
        }
    }

    private void writeGamePathFile(String path) {
        File extDir = getExternalFilesDir(null);
        if (extDir == null) {
            return;
        }
        if (!extDir.exists()) {
            extDir.mkdirs();
        }
        File out = new File(extDir, "game_path.txt");
        try (FileOutputStream fos = new FileOutputStream(out, false)) {
            fos.write((path + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {
        }
    }

    private void openAllFilesSettings() {
        mAwaitingSettingsReturn = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                startActivityForResult(intent, REQ_ALL_FILES_ACCESS);
                return;
            } catch (Exception ignored) {
            }
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                startActivityForResult(intent, REQ_ALL_FILES_ACCESS);
                return;
            } catch (Exception ignored) {
            }
        }
        mAwaitingSettingsReturn = false;
        finishPrompt("RETRY");
    }

    private void showDialogOnUiThread(final String title, final String message, final boolean showGrantStorage) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isFinishing()) {
                    finishPrompt("EXIT");
                    return;
                }
                if (mActiveDialog != null && mActiveDialog.isShowing()) {
                    try {
                        mActiveDialog.dismiss();
                    } catch (Exception ignored) {
                    }
                }
                AlertDialog.Builder builder = new AlertDialog.Builder(
                    OutRunActivity.this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
                builder.setTitle(title);
                builder.setMessage(message);
                builder.setCancelable(false);
                builder.setPositiveButton("Choose Folder", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        try {
                            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                            startActivityForResult(intent, REQ_PICK_GAME_DIR);
                        } catch (Exception e) {
                            finishPrompt("RETRY");
                        }
                    }
                });
                if (showGrantStorage) {
                    builder.setNeutralButton("All Files Access", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            openAllFilesSettings();
                        }
                    });
                } else {
                    builder.setNeutralButton("Retry", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            finishPrompt("RETRY");
                        }
                    });
                }
                builder.setNegativeButton("Exit", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        finishPrompt("EXIT");
                    }
                });
                mActiveDialog = builder.create();
                mActiveDialog.show();
            }
        });
    }

    public static boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    public static String showStartupPrompt(final String title, final String message, final boolean showGrantStorage) {
        final OutRunActivity activity = sInstance;
        if (activity == null || activity.isFinishing()) {
            return "EXIT";
        }
        synchronized (activity.mPromptLock) {
            activity.mPromptResult = null;
            activity.mLastTitle = title;
            activity.mLastMessage = message;
            activity.mLastAllowGrant = showGrantStorage;
            activity.showDialogOnUiThread(title, message, showGrantStorage);
            while (activity.mPromptResult == null && !activity.isFinishing()) {
                try {
                    activity.mPromptLock.wait(500);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return "EXIT";
                }
            }
            return activity.mPromptResult != null ? activity.mPromptResult : "EXIT";
        }
    }

    private void applyImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            View decorView = getWindow().getDecorView();
            int uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                          | View.SYSTEM_UI_FLAG_FULLSCREEN
                          | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
            decorView.setSystemUiVisibility(uiOptions);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "main"
        };
    }
}
