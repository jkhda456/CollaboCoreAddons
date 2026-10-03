import java.io.*;
import java.lang.management.ManagementFactory;
import java.net.*;
import java.net.http.*;
import java.nio.charset.Charset;
import java.nio.file.*;
import java.security.*;
import java.time.LocalDate;
import java.time.format.DateTimeFormatter;
import java.util.*;
import java.util.concurrent.*;
import java.util.stream.*;
import java.util.zip.*;
import javax.crypto.Cipher;

/** The jdk add-on's checks in the guest: one line per check, "OK name: result" or "FAIL name: error". */
public class Checks {
    static int failures;

    static void check(String name, Callable<Object> c) {
        try {
            System.out.println("OK   " + name + ": " + c.call());
        } catch (Throwable t) {
            failures++;
            System.out.println("FAIL " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static int depth;
    static void recurse() { depth++; recurse(); }
    static void deep(int n) throws Exception { if (n == 0) Thread.sleep(3000); else deep(n - 1); }

    public static void main(String[] args) throws Exception {
        String url = args.length > 0 ? args[0] : null;
        String tlsUrl = args.length > 1 ? args[1] : null;
        check("collections and streams", () -> {
            Map<String, Integer> m = new TreeMap<>();
            for (String s : "b a c a b a".split(" ")) m.merge(s, 1, Integer::sum);
            int sum = IntStream.rangeClosed(1, 1000).boxed().collect(Collectors.summingInt(x -> x));
            expect(m.toString().equals("{a=3, b=2, c=1}") && sum == 500500, m + " " + sum);
            return m + " " + sum;
        });
        check("arithmetic", () -> {
            expect(Integer.MIN_VALUE / -1 == Integer.MIN_VALUE && Long.MIN_VALUE % -1 == 0, "overflow");
            expect((int) Double.NaN == 0 && (long) 1e30 == Long.MAX_VALUE && (int) -1e30f == Integer.MIN_VALUE, "d2i");
            try { int z = 0; System.out.println(1 / z); throw new AssertionError("no exception"); }
            catch (ArithmeticException e) { /* expected */ }
            return String.format("%.4f %s %d", Math.sqrt(2), Math.floorMod(-7, 3), Long.MAX_VALUE >>> 3);
        });
        check("exceptions and stack traces", () -> {
            StackTraceElement[] st = new Throwable().getStackTrace();
            expect(st.length >= 2 && st[0].getMethodName().startsWith("lambda"), Arrays.toString(st));
            return st.length + " frames";
        });
        check("stack overflow", () -> {
            try { recurse(); } catch (StackOverflowError e) { return "StackOverflowError at depth " + depth; }
            throw new AssertionError("no StackOverflowError");
        });
        check("threads", () -> {
            ExecutorService ex = Executors.newFixedThreadPool(4);
            List<Future<Integer>> fs = new ArrayList<>();
            for (int i = 0; i < 16; i++) { int k = i; fs.add(ex.submit(() -> k * k)); }
            int s = 0;
            for (Future<Integer> f : fs) s += f.get();
            ex.shutdown();
            expect(s == 1240, "sum " + s);
            return "sum of squares " + s;
        });
        check("virtual threads", () -> {
            try (var ex = Executors.newVirtualThreadPerTaskExecutor()) {
                List<Future<Boolean>> fs = new ArrayList<>();
                for (int i = 0; i < 100; i++) fs.add(ex.submit(() -> Thread.currentThread().isVirtual()));
                for (Future<Boolean> f : fs) expect(f.get(), "not virtual");
            }
            return "100 virtual threads";
        });
        check("thread dump", () -> {
            Thread t = new Thread(() -> { try { deep(5); } catch (Exception e) { } }, "sleeper");
            t.start();
            Thread.sleep(500);
            StackTraceElement[] st = Thread.getAllStackTraces().get(t);
            t.interrupt();
            expect(st != null && st.length >= 8, "frames " + (st == null ? null : st.length));
            return st.length + " frames of the sleeping thread";
        });
        check("files", () -> {
            Path d = Files.createTempDirectory("jdk-check");
            Path f = d.resolve("한글.txt");
            Files.writeString(f, "héllo, 세계\n");
            String back = Files.readString(f).trim();
            long n;
            try (Stream<Path> s = Files.list(d)) { n = s.count(); }
            Files.delete(f);
            Files.delete(d);
            expect(back.equals("héllo, 세계") && n == 1, back);
            return back;
        });
        check("zip", () -> {
            var bo = new ByteArrayOutputStream();
            try (var z = new GZIPOutputStream(bo)) { z.write("x".repeat(10000).getBytes()); }
            byte[] c = bo.toByteArray();
            int back = new GZIPInputStream(new ByteArrayInputStream(c)).readAllBytes().length;
            expect(back == 10000, "back " + back);
            return "10000 -> " + c.length + " bytes";
        });
        check("process", () -> {
            Process p = new ProcessBuilder("sh", "-c", "echo out; echo err >&2; exit 3").start();
            String out = new String(p.getInputStream().readAllBytes()).trim();
            String err = new String(p.getErrorStream().readAllBytes()).trim();
            int rc = p.waitFor();
            expect(out.equals("out") && err.equals("err") && rc == 3, out + "/" + err + "/" + rc);
            return "sh: " + out + ", " + err + ", exit " + rc;
        });
        check("sockets", () -> {
            try (ServerSocket ss = new ServerSocket(0, 50, InetAddress.getLoopbackAddress())) {
                int port = ss.getLocalPort();
                Thread t = new Thread(() -> {
                    try (Socket c = ss.accept()) { c.getOutputStream().write("pong".getBytes()); } catch (IOException e) { }
                });
                t.start();
                try (Socket s = new Socket(InetAddress.getLoopbackAddress(), port)) {
                    String r = new String(s.getInputStream().readAllBytes());
                    expect(r.equals("pong"), r);
                    return r;
                }
            }
        });
        if (url != null) check("http client", () -> {
            HttpResponse<String> r = HttpClient.newHttpClient().send(
                HttpRequest.newBuilder(URI.create(url)).build(), HttpResponse.BodyHandlers.ofString());
            expect(r.statusCode() == 200 && r.body().contains("jdk-check"), r.statusCode() + " " + r.body());
            return r.statusCode() + " " + r.body().trim();
        });
        if (tlsUrl != null) check("https client (TLS 1.3)", () -> {
            HttpResponse<String> r = HttpClient.newHttpClient().send(
                HttpRequest.newBuilder(URI.create(tlsUrl)).build(), HttpResponse.BodyHandlers.ofString());
            String tls = r.sslSession().map(s -> s.getProtocol() + " " + s.getCipherSuite()).orElse("no TLS");
            expect(r.statusCode() == 200 && r.body().contains("jdk-check") && tls.startsWith("TLSv1.3"), r.statusCode() + " " + tls);
            return r.statusCode() + " " + tls;
        });
        check("locale and charsets", () -> {
            String n = String.format(Locale.KOREA, "%,d", 1234567);
            String d = DateTimeFormatter.ofPattern("yyyy년 M월 d일 EEEE", Locale.KOREAN).format(LocalDate.of(2026, 9, 30));
            String e = new String("한글".getBytes("EUC-KR"), "EUC-KR");
            expect(d.equals("2026년 9월 30일 수요일") && e.equals("한글"), d + " " + e);
            return n + " " + d + " " + e + " " + Charset.defaultCharset();
        });
        check("crypto", () -> {
            byte[] h = MessageDigest.getInstance("SHA-256").digest("abc".getBytes());
            String hex = HexFormat.of().formatHex(h);
            expect(hex.startsWith("ba7816bf"), hex);
            Cipher.getInstance("AES/GCM/NoPadding");
            KeyPairGenerator.getInstance("EC").generateKeyPair();
            return "sha-256 " + hex.substring(0, 16) + "..., AES/GCM, EC";
        });
        check("management", () -> {
            var rt = ManagementFactory.getRuntimeMXBean();
            return rt.getVmName() + ", " + ManagementFactory.getMemoryMXBean().getHeapMemoryUsage().getMax() / (1 << 20) + " MB heap max";
        });
        check("busy thread", () -> {
            // a thread looping without calls must not keep the rest from running (no preemption in the guest)
            Thread b = new Thread(() -> { long x = 0; while (!Thread.currentThread().isInterrupted()) x++; }, "busy");
            b.setDaemon(true);
            b.start();
            long t0 = System.nanoTime();
            Thread.sleep(200);
            System.gc();
            long ms = (System.nanoTime() - t0) / 1_000_000;
            b.interrupt();
            expect(ms < 20000, ms + " ms");
            return "sleep and GC beside it: " + ms + " ms";
        });
        System.out.println(failures == 0 ? "ALL OK" : failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
