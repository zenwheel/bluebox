import Testing
import Toolbox

extension BlueBox {
    /// Alerts that ask for another screen depth, read from their text.
    @Suite struct DepthAlerts {
        func depths(_ message: String) -> [Int] {
            let set = machine_alert_depths(message)
            return [1, 2, 4, 8, 16, 32].filter { set & (1 << UInt64($0)) != 0 }
        }

        @Test func namedDepths() {
            #expect(depths("Please set your monitor to 256 Colors.") == [8])
            #expect(depths("Glypha II requires the monitor set to 16 colors (4 bit).  Relaunch Glypha when you have switched to 16 colors.") == [4])
            #expect(depths("Room Editor requires 2-color (black & white) or 16-color mode.") == [1, 4])
            #expect(depths("Prince of Persia only runs inBlack and White, 4 color mode, 16 color mode or 256 color mode.") == [1, 2, 4, 8])
            #expect(depths("Sorry, Solarian II couldn't find an 640 by 480 or larger monitor in 8-bit (256 shade) mode.") == [8])
            #expect(depths("The depth of the screen is greater than 8 bit (256 colors).") == [8])
            #expect(depths("You must set the main screen to two colors (using the Control Panel) before running Dark Castle.") == [1])
        }

        @Test func otherRequirementsAreNotDepths() {
            #expect(depths("You must have System 6.0.7 or later and 32 Bit Quickdraw on a color Macintosh.").isEmpty)
            #expect(depths("Sorry, this program needs color to run properly.").isEmpty)
            #expect(depths("Put the '32-bit Quickdraw' file in your System folder.").isEmpty)
            #expect(depths("The graphics file could not be opened.").isEmpty)
        }
    }
}
