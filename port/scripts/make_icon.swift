// Builds port/src/macos/icon/AppIcon.icns from the 32x32 pixel-art icon,
// scaling with nearest neighbour so every pixel stays a sharp square. Every
// size is stored as PNG: iconutil packs 16 and 32 px in the old RLE format,
// which garbled a run of pixels in the 32 px icon.
//
// usage: swift port/scripts/make_icon.swift port/src/macos/icon/icon_32.png port/src/macos/icon/AppIcon.icns
import AppKit

let args = CommandLine.arguments
guard args.count == 3, let source = NSImage(contentsOfFile: args[1]),
      let cg = source.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
    FileHandle.standardError.write("usage: make_icon.swift icon.png out.icns\n".data(using: .utf8)!)
    exit(1)
}

func png(_ size: Int) -> Data {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .none
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: size, height: size))
    return NSBitmapImageRep(cgImage: ctx.makeImage()!).representation(using: .png, properties: [:])!
}

func be32(_ v: Int) -> Data { withUnsafeBytes(of: UInt32(v).bigEndian) { Data($0) } }

// icns element types that hold PNG data, by pixel size (ic11-ic14 are the @2x of 16, 32, 128, 256).
let types: [(String, Int)] = [
    ("icp4", 16), ("icp5", 32), ("ic07", 128), ("ic08", 256), ("ic09", 512), ("ic10", 1024),
    ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512),
]
var body = Data()
for (type, size) in types {
    let data = png(size)
    body += type.data(using: .ascii)! + be32(data.count + 8) + data
}
let icns = "icns".data(using: .ascii)! + be32(body.count + 8) + body
try! icns.write(to: URL(fileURLWithPath: args[2]))
