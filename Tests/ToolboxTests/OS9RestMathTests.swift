import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// MathLib's dcomplex routines.
    @Suite struct OS9RestMath {
        init() { bootMachine() }

        func cw(_ re: Double, _ im: Double) -> [UInt32] { Rest.words(re) + Rest.words(im) }

        /// A complex function of complex arguments; the result.
        func cx(_ name: String, _ args: [(Double, Double)], extra: [UInt32] = [], fp: [Double] = []) -> (Double, Double) {
            let out = Rest.ptr(16)
            Rest.call(name, [out] + args.flatMap { cw($0.0, $0.1) } + extra, fp: fp)
            return (Rest.rdDouble(out), Rest.rdDouble(out + 8))
        }

        func near(_ a: (Double, Double), _ re: Double, _ im: Double) -> Bool {
            abs(a.0 - re) < 1e-12 && abs(a.1 - im) < 1e-12
        }

        @Test func arithmetic() {
            #expect(near(cx("cadd", [(1, 2), (3, -5)]), 4, -3))
            #expect(near(cx("csub", [(1, 2), (3, -5)]), -2, 7))
            #expect(near(cx("cmul", [(1, 2), (3, -5)]), 13, 1))
            #expect(near(cx("cdiv", [(13, 1), (3, -5)]), 1, 2))
            #expect(near(cx("cconj", [(1, 2)]), 1, -2))
            #expect(near(cx("csquare", [(1, 2)]), -3, 4))
            // the real over a complex: 1 / (3 - 4i) = (3 + 4i) / 25
            #expect(near(cx("xdivc", [], extra: [0, 0] + cw(3, -4), fp: [1]), 0.12, 0.16))
        }

        @Test func magnitudeAndArgument() {
            Rest.call("cabs", cw(3, 4))
            #expect(Rest.f1() == 5)
            Rest.call("carg", cw(0, 2))
            #expect(abs(Rest.f1() - .pi / 2) < 1e-15)
            Rest.call("carg", cw(-1, 0))
            #expect(abs(Rest.f1() - .pi) < 1e-15)
        }

        @Test func transcendentals() {
            #expect(near(cx("cexp", [(0, Double.pi)]), -1, 0))
            #expect(near(cx("cLog", [(-1, 0)]), 0, .pi))
            #expect(near(cx("csqrt", [(-4, 0)]), 0, 2))
            #expect(near(cx("csin", [(0, 1)]), 0, sinh(1)))
            #expect(near(cx("ccos", [(0, 1)]), cosh(1), 0))
            #expect(near(cx("csinh", [(1, 0)]), sinh(1), 0))
            #expect(near(cx("ccosh", [(1, 0)]), cosh(1), 0))
            #expect(near(cx("ctanh", [(1, 0)]), tanh(1), 0))
            #expect(near(cx("ctan", [(1, 0)]), tan(1), 0))
            #expect(near(cx("catan", [(1, 0)]), .pi / 4, 0))
            #expect(near(cx("casin", [(1, 0)]), .pi / 2, 0))
            #expect(near(cx("cacos", [(1, 0)]), 0, 0))
            #expect(near(cx("casinh", [(1, 0)]), asinh(1), 0))
            #expect(near(cx("cacosh", [(1, 0)]), 0, 0))
            #expect(near(cx("catanh", [(0.5, 0)]), atanh(0.5), 0))
        }

        @Test func powers() {
            #expect(near(cx("cxpwri", [(1, 1)], extra: [4]), -4, 0))
            #expect(near(cx("cxpwri", [(0, 2)], extra: [UInt32(bitPattern: -2)]), -0.25, 0))
            #expect(near(cx("cxpwri", [(7, 3)], extra: [0]), 1, 0))
            #expect(near(cx("cxpwre", [(-4, 0)], fp: [0.5]), 0, 2))
            // z^w with w parked in r8-r10 and the parameter area
            #expect(near(cx("cxpwry", [(0, 1), (0, 1)]), exp(-Double.pi / 2), 0))
            #expect(near(cx("cepwry", [], extra: [0, 0] + cw(3, 0), fp: [2]), 8, 0))
            #expect(near(cx("cepwry", [], extra: [0, 0] + cw(0, 0), fp: [0]), 1, 0))
        }
    }
}
