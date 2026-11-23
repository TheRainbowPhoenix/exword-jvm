/** Public-domain smoke program for C VM classfile loading + BTU execution. */
public final class BtuSmoke {
    public static void main(String[] args) {
        int sum = 0;
        for (int i = 0; i < 100; ++i) sum += i;
        long squared = 0;
        for (int i = 0; i < 100; ++i) squared += (long)i * i;
        System.out.println("Hello from EXWord JVM");
        System.out.println("6 * 7 = " + (6 * 7));
        System.out.println("sum = " + sum);
        System.out.println("squares = " + squared);
        System.out.println("args = " + args.length);
    }
}
