package com.iitr.fieldtracker;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.AtomicFile;
import android.util.Base64;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import org.json.JSONObject;

@CapacitorPlugin(name = "ViewerTokenStorage")
public class ViewerTokenStoragePlugin extends Plugin {
    private static final String ALIAS = "field-tracker-view-token";

    private AtomicFile file() {
        return new AtomicFile(new File(getContext().getNoBackupFilesDir(), "viewer-token.json"));
    }

    private KeyStore keyStore() throws Exception {
        KeyStore keys = KeyStore.getInstance("AndroidKeyStore");
        keys.load(null);
        return keys;
    }

    private SecretKey encryptionKey() throws Exception {
        KeyStore keys = keyStore();
        if (keys.containsAlias(ALIAS)) return (SecretKey) keys.getKey(ALIAS, null);
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build());
        return generator.generateKey();
    }

    @PluginMethod
    public synchronized void get(PluginCall call) {
        AtomicFile storage = file();
        if (!storage.getBaseFile().exists()) {
            call.resolve(new JSObject().put("token", ""));
            return;
        }
        try {
            JSONObject saved = new JSONObject(new String(storage.readFully(), StandardCharsets.UTF_8));
            SecretKey key = (SecretKey) keyStore().getKey(ALIAS, null);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key,
                    new GCMParameterSpec(128, Base64.decode(saved.getString("iv"), Base64.NO_WRAP)));
            byte[] plaintext = cipher.doFinal(Base64.decode(saved.getString("data"), Base64.NO_WRAP));
            call.resolve(new JSObject().put("token", new String(plaintext, StandardCharsets.UTF_8)));
        } catch (Exception error) {
            storage.delete();
            call.reject("Saved login unavailable. Please connect again.");
        }
    }

    @PluginMethod
    public synchronized void set(PluginCall call) {
        String token = call.getString("token");
        if (token == null || token.isEmpty() || token.length() > 4096) {
            call.reject("Invalid viewer token.");
            return;
        }
        AtomicFile storage = file();
        FileOutputStream output = null;
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, encryptionKey());
            byte[] ciphertext = cipher.doFinal(token.getBytes(StandardCharsets.UTF_8));
            JSONObject saved = new JSONObject()
                    .put("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                    .put("data", Base64.encodeToString(ciphertext, Base64.NO_WRAP));
            output = storage.startWrite();
            output.write(saved.toString().getBytes(StandardCharsets.UTF_8));
            storage.finishWrite(output);
            call.resolve();
        } catch (Exception error) {
            if (output != null) storage.failWrite(output);
            call.reject("Could not save login on this device.");
        }
    }

    @PluginMethod
    public synchronized void remove(PluginCall call) {
        file().delete();
        try {
            keyStore().deleteEntry(ALIAS);
            call.resolve();
        } catch (Exception error) {
            call.reject("Could not clear saved login. Please try Disconnect again.");
        }
    }
}
