import BlueBoxHost
import Testing

/// run --app: which of an archive's applications a name picks.
@Suite struct AppMatch {
    let paths = [
        "JChecker 2.0.2/JChecker2",
        "JChecker 2.0.2/JChecker2.0 Manual",
        "Agenda/Agenda (Grape)/Register",
        "Agenda/Agenda (Lime)/Register",
        "Cruciverbalist/Register Cruciverbalist",
        "AutoShare/Write Log/Unsubscribe",
        "AutoShare/Scripts/unsubscribe",
        "Escape/  READ ME  ",
        "Caf\u{00E9} Game/Caf\u{00E9}",
    ]

    @Test func wholeNameBeforePartOfOne() {
        #expect(matchApplications(paths, wanted: "JChecker2") == [0])
        #expect(matchApplications(paths, wanted: "jchecker2") == [0])
        // A part of a name still picks, when it is the only kind of match.
        #expect(matchApplications(paths, wanted: "Manual") == [1])
        #expect(matchApplications(paths, wanted: "Cruciverbalist") == [4])
    }

    @Test func pathTellsApartTheSameName() {
        #expect(matchApplications(paths, wanted: "Register") == [2, 3])
        #expect(matchApplications(paths, wanted: "Agenda/Agenda (Lime)/Register") == [3])
        #expect(matchApplications(paths, wanted: "Agenda (Grape)/Register") == [2])
        #expect(matchApplications(paths, wanted: "agenda (grape)/register") == [2])
        #expect(matchApplications(paths, wanted: "Plum)/Register").isEmpty)
    }

    @Test func caseOnlyWhenItDecides() {
        #expect(matchApplications(paths, wanted: "Unsubscribe") == [5])
        #expect(matchApplications(paths, wanted: "unsubscribe") == [6])
        #expect(matchApplications(paths, wanted: "UNSUBSCRIBE") == [5, 6])
    }

    @Test func spacesAndNormalisation() {
        #expect(matchApplications(paths, wanted: "  READ ME  ") == [7])
        #expect(matchApplications(paths, wanted: "READ ME") == [7])
        // Decomposed, as macOS file names often are.
        #expect(matchApplications(paths, wanted: "Cafe\u{0301}") == [8])
        #expect(matchApplications(paths, wanted: "Cafe\u{0301} Game/Caf\u{00E9}") == [8])
    }
}
