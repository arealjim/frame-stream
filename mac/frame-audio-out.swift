// frame-audio-out - read or set the Mac's default sound output, so an engine started after it plays into BlackHole 2ch
// (and from there to the headset) instead of the Mac's speakers. frame-stream compiles it on first use:
//   swiftc -O -o frame-audio-out frame-audio-out.swift
// Usage: frame-audio-out get              the default output, as "<uid>\t<name>"
//        frame-audio-out set <uid|name>   make that device the default output (exit 1 if no output device matches)
//        frame-audio-out has <uid|name>   exit 0 if an output device matches
// Godot, Unity and Unreal all play to the default output and pick it when they start, so frame-stream switches it
// before the engine launches and puts the old one back on down.
import CoreAudio
import Foundation

let system = AudioObjectID(kAudioObjectSystemObject)

func address(_ selector: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}

func devices() -> [AudioDeviceID] {
    var a = address(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(system, &a, 0, nil, &size) == noErr else { return [] }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(system, &a, 0, nil, &size, &ids) == noErr else { return [] }
    return ids
}

func text(_ id: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String {
    var a = address(selector)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, &value) == noErr, let v = value else { return "" }
    return v.takeRetainedValue() as String
}

func hasOutput(_ id: AudioDeviceID) -> Bool {
    var a = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput)
    var size: UInt32 = 0
    return AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr && size > 0
}

func find(_ key: String) -> AudioDeviceID? {
    let outs = devices().filter(hasOutput)
    return outs.first { text($0, kAudioDevicePropertyDeviceUID) == key } ?? outs.first { text($0, kAudioObjectPropertyName) == key }
}

func defaultOutput() -> AudioDeviceID {
    var a = address(kAudioHardwarePropertyDefaultOutputDevice)
    var id = AudioDeviceID(0)
    var size = UInt32(MemoryLayout<AudioDeviceID>.size)
    AudioObjectGetPropertyData(system, &a, 0, nil, &size, &id)
    return id
}

let args = CommandLine.arguments
switch (args.count > 1 ? args[1] : "", args.count > 2 ? args[2] : nil) {
case ("get", nil):
    let id = defaultOutput()
    print("\(text(id, kAudioDevicePropertyDeviceUID))\t\(text(id, kAudioObjectPropertyName))")
case ("has", let key?):
    exit(find(key) == nil ? 1 : 0)
case ("set", let key?):
    guard var id = find(key) else { FileHandle.standardError.write("frame-audio-out: no output device '\(key)'\n".data(using: .utf8)!); exit(1) }
    var a = address(kAudioHardwarePropertyDefaultOutputDevice)
    let r = AudioObjectSetPropertyData(system, &a, 0, nil, UInt32(MemoryLayout<AudioDeviceID>.size), &id)
    guard r == noErr else { FileHandle.standardError.write("frame-audio-out: setting the default output failed (\(r))\n".data(using: .utf8)!); exit(1) }
default:
    FileHandle.standardError.write("usage: frame-audio-out get | set <uid|name> | has <uid|name>\n".data(using: .utf8)!)
    exit(2)
}
