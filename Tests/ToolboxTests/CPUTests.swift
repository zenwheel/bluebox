import CPUTestSupport
import Foundation
import Testing

extension Classic {
    /// Runs the SingleStepTests 68000 vectors, if Tests/fetch-cpu-tests.sh has fetched them.
    @Suite struct CPUSingleStepTests {
        static let cacheDir: URL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent(".cache/sst")

        static var files: [String] {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: cacheDir.path)) ?? []
            return names.filter { $0.hasSuffix(".json") }.sorted()
        }

        @Test("68000 single-step vectors", arguments: files)
        func vectors(file: String) throws {
            var result = sst_result()
            let path = Self.cacheDir.appendingPathComponent(file).path
            #expect(sst_run_file(path, &result), "could not parse \(file)")
            let failure = withUnsafeBytes(of: &result.first_failure) { raw in
                String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
            }
            #expect(result.failed == 0, "\(file): \(result.failed)/\(result.total) failed; first: \(failure)")
        }
    }
}
