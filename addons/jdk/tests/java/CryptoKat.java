import java.io.ByteArrayOutputStream;
import java.io.PrintWriter;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Random;
import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

// Known answers for the methods Zero runs in C (zeroIntrinsics_zero.cpp in patch 0008): digests,
// AES in ECB, CBC, CTR and GCM, GHASH, Math.multiplyHigh, BigInteger arithmetic, EC and RSA keys
// and signatures from seeded generators. Writes one line per result to args[0], which must equal
// tests/crypto-kat.txt (written by this program on a desktop JDK 25), and its times to stdout.
public class CryptoKat {
    static PrintWriter out;

    static byte[] data(int n, int seed) {
        byte[] b = new byte[n];
        int x = seed;
        for (int i = 0; i < n; i++) {
            x = x * 1103515245 + 12345;
            b[i] = (byte) (x >>> 16);
        }
        return b;
    }

    static String hex(byte[] b) throws Exception {
        return b.length <= 32 ? HexFormat.of().formatHex(b)
                : HexFormat.of().formatHex(MessageDigest.getInstance("SHA-1").digest(b)) + "/" + b.length;
    }

    static void put(String name, byte[] b) throws Exception {
        out.println(name + " " + hex(b));
    }

    static SecureRandom seeded(int seed) throws Exception {
        SecureRandom r = SecureRandom.getInstance("SHA1PRNG");
        r.setSeed(seed);
        return r;
    }

    static void digests() throws Exception {
        int[] lengths = {0, 3, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000, 100003};
        for (String alg : new String[] {"MD5", "SHA-1", "SHA-224", "SHA-256", "SHA-384", "SHA-512", "SHA-512/256"}) {
            for (int n : lengths) {
                byte[] d = data(n, n + 1);
                MessageDigest md = MessageDigest.getInstance(alg);
                byte[] whole = md.digest(d);
                for (int i = 0; i < n; i += 7) {
                    md.update(d, i, Math.min(7, n - i));
                }
                byte[] chunked = md.digest();
                md.update(d, 0, n / 3);
                md.update(ByteBuffer.wrap(d, n / 3, n - n / 3));
                byte[] split = md.digest();
                boolean same = Arrays.equals(whole, chunked) && Arrays.equals(whole, split);
                put(alg + " " + n + (same ? "" : " MISMATCH"), whole);
            }
        }
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(data(32, 5), "HmacSHA256"));
        put("hmac-sha256", mac.doFinal(data(5000, 6)));
    }

    static void aes() throws Exception {
        for (int bytes : new int[] {16, 24, 32}) {
            String name = "aes" + bytes * 8;
            SecretKeySpec key = new SecretKeySpec(data(bytes, bytes), "AES");
            byte[] msg = data(160, 7);
            Cipher ecb = Cipher.getInstance("AES/ECB/NoPadding");
            ecb.init(Cipher.ENCRYPT_MODE, key);
            byte[] ct = ecb.doFinal(msg);
            put(name + "-ecb", ct);
            ecb.init(Cipher.DECRYPT_MODE, key);
            out.println(name + "-ecb-decrypt " + Arrays.equals(ecb.doFinal(ct), msg));

            Cipher cbc = Cipher.getInstance("AES/CBC/PKCS5Padding");
            IvParameterSpec iv = new IvParameterSpec(data(16, 9));
            cbc.init(Cipher.ENCRYPT_MODE, key, iv);
            ct = cbc.doFinal(data(1001, 8));
            put(name + "-cbc", ct);
            cbc.init(Cipher.DECRYPT_MODE, key, iv);
            put(name + "-cbc-decrypt", cbc.doFinal(ct));

            // A counter that carries into its higher bytes, fed 13 bytes at a time
            Cipher ctr = Cipher.getInstance("AES/CTR/NoPadding");
            byte[] counter = data(16, 10);
            counter[14] = (byte) 0xff;
            counter[15] = (byte) 0xfe;
            ctr.init(Cipher.ENCRYPT_MODE, key, new IvParameterSpec(counter));
            byte[] in = data(1000, 11);
            ByteArrayOutputStream stream = new ByteArrayOutputStream();
            for (int i = 0; i < in.length; i += 13) {
                byte[] r = ctr.update(in, i, Math.min(13, in.length - i));
                if (r != null) {
                    stream.write(r);
                }
            }
            stream.write(ctr.doFinal());
            put(name + "-ctr", stream.toByteArray());

            for (int n : new int[] {0, 15, 16, 17, 1000, 70001}) {
                Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
                GCMParameterSpec spec = new GCMParameterSpec(128, data(12, n + 3));
                gcm.init(Cipher.ENCRYPT_MODE, key, spec);
                gcm.updateAAD(data(20, 4));
                byte[] sealed = gcm.doFinal(data(n, n + 2));
                put(name + "-gcm " + n, sealed);
                gcm.init(Cipher.DECRYPT_MODE, key, spec);
                gcm.updateAAD(data(20, 4));
                put(name + "-gcm-decrypt " + n, gcm.doFinal(sealed));
                ByteBuffer src = ByteBuffer.allocateDirect(sealed.length).put(sealed).flip();
                ByteBuffer dst = ByteBuffer.allocateDirect(n + 16);
                gcm.init(Cipher.DECRYPT_MODE, key, spec);
                gcm.updateAAD(data(20, 4));
                gcm.doFinal(src, dst);
                byte[] opened = new byte[dst.flip().remaining()];
                dst.get(opened);
                put(name + "-gcm-direct " + n, opened);
                if (n > 0) {
                    sealed[0] ^= 1;
                    gcm.init(Cipher.DECRYPT_MODE, key, spec);
                    gcm.updateAAD(data(20, 4));
                    try {
                        gcm.doFinal(sealed);
                        out.println(name + "-gcm-tampered " + n + " accepted");
                    } catch (AEADBadTagException e) {
                        out.println(name + "-gcm-tampered " + n + " rejected");
                    }
                }
            }
        }
    }

    static void numbers() throws Exception {
        Random r = new Random(42);
        long high = 0, unsignedHigh = 0;
        for (int i = 0; i < 20000; i++) {
            long x = i % 13 == 0 ? 0 : i % 7 == 0 ? -1 : r.nextLong();
            long y = i % 11 == 0 ? Long.MIN_VALUE : r.nextLong();
            high = high * 31 + Math.multiplyHigh(x, y);
            unsignedHigh = unsignedHigh * 31 + Math.unsignedMultiplyHigh(x, y);
        }
        out.println("multiplyHigh " + high + " " + unsignedHigh);
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        for (int bits : new int[] {31, 32, 33, 64, 100, 512, 1000, 1024, 2048, 3000, 4096}) {
            BigInteger x = new BigInteger(bits, r), y = new BigInteger(bits + 17, r);
            BigInteger m = new BigInteger(bits, r).setBit(0).setBit(bits - 1);
            BigInteger e = new BigInteger(Math.min(bits, 300), r);
            md.update(x.multiply(y).toByteArray());
            md.update(x.multiply(x).toByteArray());
            md.update(x.pow(3).toByteArray());
            md.update(x.modPow(e, m).toByteArray());
            md.update(y.modPow(e, m.add(BigInteger.ONE)).toByteArray());
            md.update(y.mod(m).toByteArray());
            put("BigInteger " + bits, md.digest());
        }
    }

    static void keys() throws Exception {
        for (String curve : new String[] {"secp256r1", "secp384r1", "secp521r1"}) {
            KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
            g.initialize(new ECGenParameterSpec(curve), seeded(1));
            KeyPair k1 = g.generateKeyPair(), k2 = g.generateKeyPair();
            put(curve + " public", k1.getPublic().getEncoded());
            put(curve + " public2", k2.getPublic().getEncoded());
            KeyAgreement ka = KeyAgreement.getInstance("ECDH");
            ka.init(k1.getPrivate());
            ka.doPhase(k2.getPublic(), true);
            put(curve + " ecdh", ka.generateSecret());
            Signature s = Signature.getInstance("SHA256withECDSA");
            s.initSign(k1.getPrivate(), seeded(2));
            s.update("message".getBytes());
            byte[] sig = s.sign();
            put(curve + " ecdsa", sig);
            s.initVerify(k1.getPublic());
            s.update("message".getBytes());
            out.println(curve + " ecdsa-verify " + s.verify(sig));
            s.initVerify(k1.getPublic());
            s.update("massage".getBytes());
            out.println(curve + " ecdsa-verify-other " + s.verify(sig));
        }
        KeyPairGenerator xg = KeyPairGenerator.getInstance("X25519");
        xg.initialize(255, seeded(3));
        KeyPair x1 = xg.generateKeyPair(), x2 = xg.generateKeyPair();
        KeyAgreement xa = KeyAgreement.getInstance("X25519");
        xa.init(x1.getPrivate());
        xa.doPhase(x2.getPublic(), true);
        put("x25519", xa.generateSecret());

        KeyPairGenerator rg = KeyPairGenerator.getInstance("RSA");
        rg.initialize(2048, seeded(4));
        KeyPair rk = rg.generateKeyPair();
        put("rsa2048 public", rk.getPublic().getEncoded());
        Signature rs = Signature.getInstance("SHA256withRSA");
        rs.initSign(rk.getPrivate());
        rs.update("message".getBytes());
        byte[] sig = rs.sign();
        put("rsa2048 signature", sig);
        rs.initVerify(rk.getPublic());
        rs.update("message".getBytes());
        out.println("rsa2048 verify " + rs.verify(sig));
    }

    public static void main(String[] args) throws Exception {
        try (PrintWriter w = new PrintWriter(args[0])) {
            out = w;
            for (String part : new String[] {"digests", "aes", "numbers", "keys"}) {
                long t = System.nanoTime();
                switch (part) {
                    case "digests" -> digests();
                    case "aes" -> aes();
                    case "numbers" -> numbers();
                    default -> keys();
                }
                System.out.printf("%s %d ms%n", part, (System.nanoTime() - t) / 1000000);
            }
        }
    }
}
