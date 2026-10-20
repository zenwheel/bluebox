// Game controllers for InputSprocket: each controller GameController.framework
// finds (up to four) has a slot, and its buttons, sticks and triggers go to
// the machine (machine_set_gamepad) whenever they change.  The first one
// connected is player 1's.

#if canImport(GameController)
@preconcurrency import GameController
import Toolbox

@MainActor
public final class GameControllers {
    public static let shared = GameControllers()

    private var slots: [GCController?] = Array(repeating: nil, count: Int(MACHINE_PADS))
    private var installed = false

    /// Follow the host's controllers, connected now and later.
    public func install() {
        guard !installed else { return }
        installed = true
        // Keep reading them while another application is in front, as a
        // classic game did.
        GCController.shouldMonitorBackgroundEvents = true
        let center = NotificationCenter.default
        center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { note in
            guard let controller = note.object as? GCController else { return }
            MainActor.assumeIsolated { GameControllers.shared.connect(controller) }
        }
        center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { note in
            guard let controller = note.object as? GCController else { return }
            MainActor.assumeIsolated { GameControllers.shared.disconnect(controller) }
        }
        for controller in GCController.controllers() { connect(controller) }
    }

    private func connect(_ controller: GCController) {
        guard !slots.contains(where: { $0 === controller }),
              let slot = slots.firstIndex(where: { $0 == nil }) else { return }
        slots[slot] = controller
        controller.playerIndex = GCControllerPlayerIndex(rawValue: slot) ?? .indexUnset
        controller.handlerQueue = .main
        if let pad = controller.extendedGamepad {
            pad.valueChangedHandler = { _, _ in
                MainActor.assumeIsolated { GameControllers.shared.update(slot) }
            }
        } else if let pad = controller.microGamepad {
            pad.reportsAbsoluteDpadValues = true
            pad.valueChangedHandler = { _, _ in
                MainActor.assumeIsolated { GameControllers.shared.update(slot) }
            }
        }
        update(slot)
    }

    private func disconnect(_ controller: GCController) {
        guard let slot = slots.firstIndex(where: { $0 === controller }) else { return }
        slots[slot] = nil
        var state = machine_gamepad()
        machine_set_gamepad(Int32(slot), &state)
    }

    /// Hand the machine slot's state as it is now.
    private func update(_ slot: Int) {
        guard let controller = slots[slot] else { return }
        var state = machine_gamepad()
        state.connected = true
        let name = controller.vendorName ?? controller.productCategory
        withUnsafeMutableBytes(of: &state.name) { buf in
            let utf8 = Array(name.utf8.prefix(buf.count - 1))
            buf.copyBytes(from: utf8)
        }
        var buttons: UInt32 = 0
        func press(_ on: Bool, _ bit: Int) { if on { buttons |= UInt32(bit) } }
        var axes = [Float](repeating: 0, count: Int(MACHINE_PAD_AXES))
        if let pad = controller.extendedGamepad {
            press(pad.buttonA.isPressed, MACHINE_PAD_A)
            press(pad.buttonB.isPressed, MACHINE_PAD_B)
            press(pad.buttonX.isPressed, MACHINE_PAD_X)
            press(pad.buttonY.isPressed, MACHINE_PAD_Y)
            press(pad.leftShoulder.isPressed, MACHINE_PAD_L1)
            press(pad.rightShoulder.isPressed, MACHINE_PAD_R1)
            press(pad.leftTrigger.isPressed, MACHINE_PAD_L2)
            press(pad.rightTrigger.isPressed, MACHINE_PAD_R2)
            press(pad.leftThumbstickButton?.isPressed ?? false, MACHINE_PAD_L3)
            press(pad.rightThumbstickButton?.isPressed ?? false, MACHINE_PAD_R3)
            press(pad.buttonMenu.isPressed, MACHINE_PAD_MENU)
            press(pad.buttonOptions?.isPressed ?? false, MACHINE_PAD_OPTIONS)
            press(pad.dpad.up.isPressed, MACHINE_PAD_UP)
            press(pad.dpad.down.isPressed, MACHINE_PAD_DOWN)
            press(pad.dpad.left.isPressed, MACHINE_PAD_LEFT)
            press(pad.dpad.right.isPressed, MACHINE_PAD_RIGHT)
            axes[Int(MACHINE_PAD_LX)] = pad.leftThumbstick.xAxis.value
            axes[Int(MACHINE_PAD_LY)] = pad.leftThumbstick.yAxis.value
            axes[Int(MACHINE_PAD_RX)] = pad.rightThumbstick.xAxis.value
            axes[Int(MACHINE_PAD_RY)] = pad.rightThumbstick.yAxis.value
            axes[Int(MACHINE_PAD_LT)] = pad.leftTrigger.value
            axes[Int(MACHINE_PAD_RT)] = pad.rightTrigger.value
        } else if let pad = controller.microGamepad {
            // A Siri Remote and the like: the touch surface as a pad.
            press(pad.buttonA.isPressed, MACHINE_PAD_A)
            press(pad.buttonX.isPressed, MACHINE_PAD_X)
            press(pad.buttonMenu.isPressed, MACHINE_PAD_MENU)
            axes[Int(MACHINE_PAD_LX)] = pad.dpad.xAxis.value
            axes[Int(MACHINE_PAD_LY)] = pad.dpad.yAxis.value
        }
        state.buttons = buttons
        withUnsafeMutableBytes(of: &state.axes) { buf in
            for (i, v) in axes.enumerated() { buf.storeBytes(of: v, toByteOffset: i * 4, as: Float.self) }
        }
        machine_set_gamepad(Int32(slot), &state)
    }
}
#endif
