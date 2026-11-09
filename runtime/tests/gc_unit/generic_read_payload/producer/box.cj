package generic_read_payload

// Kept in an independently compiled library without @Frozen. The consumer
// must use its generic field reader rather than import/specialize its body.
public class Box<T> {
    // The container has a reference even when T is reference-free. The
    // runtime must inspect the destination value layout, not this class.
    public let anchor: String
    private var value: T
    public init(value: T) { this.anchor = "container anchor"; this.value = value }
    public func read(): T { return value }
}

public struct Payload {
    public let text: String
    public let number: Int64
    public init(text: String, number: Int64) {
        this.text = text
        this.number = number
    }
}
