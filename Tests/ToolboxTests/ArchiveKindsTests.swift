import BlueBoxHost
import Testing

/// Which Mac files bluebox import expands as archives.
@Suite struct ArchiveKinds {
    func code(_ s: String) -> UInt32 { s.utf8.reduce(0) { ($0 << 8) | UInt32($1) } }
    let zip: [UInt8] = [0x50, 0x4B, 0x03, 0x04, 0x14, 0x00]
    // A StuffIt archive's header: its signature, then "rLau" at 10.
    let stuffIt: [UInt8] = Array("SIT!".utf8) + [0, 1, 0, 0, 0, 0] + Array("rLau".utf8) + [1, 0]

    @Test func selfExtractorsAreArchives() {
        #expect(isMacArchive(type: code("APPL"), creator: code("STi0"), name: "Textures", data: stuffIt))
        #expect(isMacArchive(type: code("APPL"), creator: code("aust"), name: "Game", data: []))
        #expect(isMacArchive(type: code("SIT!"), creator: code("SIT!"), name: "Game", data: []))
    }

    @Test func javaApplicationIsNotAnArchive() {
        // JBindery's applications carry their classes as a zip.
        #expect(!isMacArchive(type: code("APPL"), creator: code("jtUA"), name: "Jake 1.1", data: zip))
        #expect(isMacArchive(type: code("ZIP "), creator: code("ZIP "), name: "Classes", data: zip))
    }

    @Test func otherFilesAreNot() {
        #expect(!isMacArchive(type: code("APPL"), creator: code("WILD"), name: "HyperCard", data: []))
        // Word's settings are 'SITD' too.
        #expect(!isMacArchive(type: code("SITD"), creator: code("MSWD"), name: "Word Settings", data: []))
    }
}
