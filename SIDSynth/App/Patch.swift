import Foundation

/// Ein Klang: Name und je Parameter ein Rohwert, in der Reihenfolge von params.h.
///
/// Das JSON-Format ist das der Box (presets.cpp), so lassen sich Klänge ohne Umrechnung
/// hin und her schieben: `{"fmt":1,"name":"Sägezahn-Lead","v":{"o1.wave":2,…}}`.
struct Patch: Equatable {
    var name: String
    var values: [Int16]

    static let count = Int(sid_param_count())
    static let slots = Int(sid_slots())
    static let keys: [String] = (0..<count).map { String(cString: sid_param_key(Int32($0))) }

    /// Alle Werte auf die Voreinstellung, wie init_patch().
    static func initial(name: String) -> Patch {
        Patch(name: boxName(name), values: (0..<count).map { Int16(sid_param_default(Int32($0))) })
    }

    /// Der Werksklang des Platzes (ab 0), aus factory.cpp der Firmware.
    static func factory(slot: Int) -> Patch? {
        var name = [CChar](repeating: 0, count: Int(sid_name_capacity()))
        var values = [Int16](repeating: 0, count: count)
        guard sid_factory(Int32(slot), &name, &values) else { return nil }
        return Patch(name: String(cString: name), values: values)
    }

    /// Kürzt und säubert einen Namen genau wie set_name() auf der Box.
    static func boxName(_ text: String) -> String {
        var out = [CChar](repeating: 0, count: Int(sid_name_capacity()))
        sid_set_name(text, &out)
        return String(cString: out)
    }

    static func clamp(_ index: Int, _ value: Int) -> Int16 {
        Int16(sid_clamp_value(Int32(index), value))
    }

    mutating func set(_ index: Int, _ value: Int) {
        values[index] = Patch.clamp(index, value)
    }

    // MARK: JSON

    /// Wie read_file() der Box: unbekannte Schlüssel übergehen, fehlende behalten die
    /// Voreinstellung, jeder Wert wird auf seinen Bereich begrenzt.
    init(json: [String: Any]) {
        self = Patch.initial(name: json["name"] as? String ?? "Ohne Namen")
        let v = json["v"] as? [String: Any] ?? [:]
        for (index, key) in Patch.keys.enumerated() {
            if let number = v[key] as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
               let value = Int(exactly: number) {
                set(index, value)
            }
        }
    }

    init(name: String, values: [Int16]) {
        self.name = name
        self.values = values
    }

    /// In der Reihenfolge, in der auch die Box schreibt. Mit `slot` (ab 0) wie ein
    /// Eintrag im Backup, dort als `"slot": slot + 1`.
    func json(slot: Int? = nil) -> String {
        var out = "{"
        if let slot { out += "\"slot\":\(slot + 1)," }
        out += "\"fmt\":1,\"name\":\(Patch.quoted(name)),\"v\":{"
        for (index, key) in Patch.keys.enumerated() {
            if index > 0 { out += "," }
            out += "\"\(key)\":\(values[index])"
        }
        return out + "}}"
    }

    static func quoted(_ text: String) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .withoutEscapingSlashes
        let data = (try? encoder.encode(text)) ?? Data("\"\"".utf8)
        return String(decoding: data, as: UTF8.self)
    }
}
