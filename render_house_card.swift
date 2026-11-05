import AppKit

let canvas = 1254
let output = CommandLine.arguments.dropFirst().first ?? "HOUSE_CARD.png"
let bitmap = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: canvas,
    pixelsHigh: canvas,
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
)!
let context = NSGraphicsContext(bitmapImageRep: bitmap)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
context.imageInterpolation = .high
context.cgContext.clear(CGRect(x: 0, y: 0, width: canvas, height: canvas))

let lines = ["beats&", "vibes"]
let fontSize: CGFloat = 280
let font = NSFont(name: "HelveticaNeue-Bold", size: fontSize)!
let attributes: [NSAttributedString.Key: Any] = [
    .font: font,
    .foregroundColor: NSColor.white,
    .kern: -0.105 * fontSize,
]
let rendered = lines.map { NSAttributedString(string: $0, attributes: attributes) }
let sizes = rendered.map { $0.size() }
let gap: CGFloat = -114
let totalHeight = sizes[0].height + sizes[1].height + gap
let bottom = (CGFloat(canvas) - totalHeight) / 2
let firstY = bottom + sizes[1].height + gap

for (index, text) in rendered.enumerated() {
    let x = (CGFloat(canvas) - sizes[index].width) / 2
    text.draw(at: NSPoint(x: x, y: index == 0 ? firstY : bottom))
}

context.flushGraphics()
NSGraphicsContext.restoreGraphicsState()
try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: output))
