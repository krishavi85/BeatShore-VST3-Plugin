Third-party notices for components bundled with BeatShore.

These are attribution notices, not a contract the end user is asked to
accept (see ..\LicenseFile.txt, shown during setup, for that).

  VST3-SDK-LICENSE.txt              Steinberg VST3 SDK 3.8.1 (MIT)
  JUCE-LICENSE.md                   JUCE 9.0.1 (used under the free
                                     Starter tier -- see LicenseFile.txt)
  Node.js-LICENSE.txt               Node.js runtime (MIT)
  TensorFlow-C-LICENSE.txt          TensorFlow C library, bundled via
                                     @tensorflow/tfjs-node (Apache-2.0)
  TensorFlow-C-THIRD-PARTY-NOTICES.txt
                                     Third-party notices for the libraries
                                     TensorFlow's own C build depends on
  basic-pitch-LICENSE-Apache-2.0.txt Basic Pitch model (Spotify, Apache-2.0)

@tensorflow/tfjs and @tensorflow/tfjs-node's own JavaScript layer are
also Apache-2.0 licensed (per their package.json), the same standard
license text as basic-pitch-LICENSE-Apache-2.0.txt above -- not
duplicated here as a separate file since it's the identical boilerplate.

This list was assembled from the actual files vendored in this repository
(native/vst3sdk/LICENSE.txt, native/JUCE/LICENSE.md,
vendor/basic-pitch-model/LICENSE-basic-pitch-apache-2.0.txt, and the
staged node.exe / node_modules themselves) -- not a template. Regenerate
it if any dependency is added, removed, or upgraded.
