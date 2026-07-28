#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint flutter_pty2.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'flutter_pty2'
  s.version          = '1.0.0'
  s.summary          = 'Flutter FFI pseudo-terminal plugin.'
  s.description      = <<-DESC
Flutter FFI pseudo-terminal plugin for spawning and controlling terminal processes.
                       DESC
  s.homepage         = 'https://github.com/SoFluffyOS/flutter_pty2'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'SoFluffy' => 'hi@sofluffy.io' }

  # The forwarder C file imports the shared sources from `../src/*` so both
  # CocoaPods and Swift Package Manager build the same implementation.
  s.source           = { :path => '.' }
  s.source_files = 'flutter_pty2/Sources/flutter_pty2/**/*'
  s.dependency 'Flutter'
  s.platform = :ios, '9.0'

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386' }
  s.swift_version = '5.0'
end
