import net.minecraft.util.Interval;

/** Prints float bit patterns for the interval operations the DF compiler uses. */
public class IntervalVectors {
    static void p(String name, Interval i) {
        System.out.printf("%s %08x %08x%n", name,
            Float.floatToRawIntBits(i.min()), Float.floatToRawIntBits(i.max()));
    }

    public static void main(String[] args) {
        Interval a = Interval.of(-2.0F, 3.0F);
        Interval b = Interval.of(0.5F, 4.0F);
        Interval c = Interval.of(-10.0F, -1.0F);
        Interval pos = Interval.of(0.25F, 2.0F);
        Interval spot = Interval.ofExact(2.5F);
        Interval zero = Interval.ofExact(0.0F);
        Interval one = Interval.ofExact(1.0F);
        Interval inf = Interval.INFINITE;
        Interval nai = Interval.NaI;

        p("of.exact", spot);
        p("of.symmetric", Interval.ofSymmetric(2.0F));
        p("add", Interval.add(a, b));
        p("add.nai", Interval.add(a, nai));
        p("sub", Interval.sub(a, b));
        p("mul.cross", Interval.mul(c, b));
        p("mul.zero", Interval.mul(zero, inf));
        p("div", Interval.div(a, Interval.of(1.0F, 2.0F)));
        p("div.through.zero", Interval.div(a, Interval.of(-1.0F, 1.0F)));
        p("min", Interval.min(a, b));
        p("max", Interval.max(a, b));
        p("clamp.inner", Interval.clamp(a, -1.0F, 1.0F));
        p("clamp.above", Interval.clamp(a, 10.0F, 20.0F));
        p("abs.cross", Interval.abs(c));
        p("abs.positive", Interval.abs(pos));
        p("square.cross", Interval.square(a));
        p("square.negative", Interval.square(c));
        p("pow.square", Interval.pow(a, Interval.ofExact(2.0F)));
        p("pow.negative.base", Interval.pow(Interval.of(-2.0F, -1.0F), Interval.of(-3.0F, 3.0F)));
        p("pow.zero.base", Interval.pow(zero, Interval.of(-1.0F, 2.0F)));
        p("pow.positive.base", Interval.pow(pos, Interval.of(0.5F, 3.0F)));
        p("pow.infinite.exp", Interval.pow(Interval.of(0.5F, 0.5F), inf));
        p("log", Interval.log(Interval.of(-1.0F, 4.0F)));
        p("sign.zero.edge", Interval.sign(Interval.of(-3.0F, 0.0F)));
        p("sign.span", Interval.sign(a));
        p("map.cube", Interval.mapMonotonic(c, x -> x * x * x));
        p("lerp.finite", Interval.lerp(Interval.of(0.25F, 0.75F), -2.0F, 5.0F));
        p("lerp.infinite", Interval.lerp(Interval.of(0.0F, 1.0F),
            Float.NEGATIVE_INFINITY, 3.0F));
        p("lerp.intervals", Interval.lerp(Interval.of(0.0F, 1.0F), c, b));
        p("encap.intervals", Interval.encapsulating(a, c));
        p("nai", nai);
    }
}
