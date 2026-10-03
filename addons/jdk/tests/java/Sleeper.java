/** Sleeps a while: something for jcmd and jstack to look at. */
public class Sleeper {
    public static void main(String[] args) throws Exception {
        System.out.println("sleeping");
        Thread.sleep(Long.parseLong(args.length > 0 ? args[0] : "60000"));
    }
}
