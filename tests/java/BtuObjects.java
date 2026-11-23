/** Extra public JVM acceptance test: objects, calls, fields, and arrays. */
public final class BtuObjects {
    static int counter = 5;
    int value;

    BtuObjects(int n) { this.value = n; }
    int add(int n) { return value + n; }
    static int triple(int n) { return n * 3; }

    public static void main(String[] args) {
        BtuObjects obj = new BtuObjects(7);
        int[] array = new int[4];
        for (int i = 0; i < array.length; i++)
            array[i] = i + 1;
        int sum = 0;
        for (int i = 0; i < array.length; i++)
            sum += array[i];

        System.out.println("add = " + obj.add(8));
        System.out.println("sum = " + sum);
        System.out.println("static = " + triple(counter));
    }
}
