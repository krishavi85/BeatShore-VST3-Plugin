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
  Python-LICENSE.txt                Python 3.14 (PSF License), embeddable
                                     package bundled with the optional
                                     "MT3 Model Pack" installer component
  PyTorch-LICENSE.txt               PyTorch 2.14 (BSD-3-Clause), bundled
                                     with the MT3 Model Pack
  mt3-infer-and-MR-MT3-LICENSE.txt  mt3-infer (MIT) -- carries MR-MT3's
                                     own attribution/license inline, since
                                     MR-MT3's inference code was extracted
                                     and adapted directly into mt3-infer
                                     rather than vendored as a separate
                                     package (see mt3_infer's own LICENSE
                                     file, reproduced verbatim here);
                                     bundled with the MT3 Model Pack
  MT3-ModelPack-THIRD-PARTY-NOTICES.txt
                                     Every other real dependency the MT3
                                     Model Pack's Python runtime ships
                                     (transformers, numpy, scipy, librosa,
                                     scikit-learn, soundfile, mido, and
                                     the rest -- 66 packages), generated
                                     directly from each package's own
                                     installed metadata/license file, not
                                     hand-typed

@tensorflow/tfjs and @tensorflow/tfjs-node's own JavaScript layer are
also Apache-2.0 licensed (per their package.json), the same standard
license text as basic-pitch-LICENSE-Apache-2.0.txt above -- not
duplicated here as a separate file since it's the identical boilerplate.

This list was assembled from the actual files vendored in this repository
(native/vst3sdk/LICENSE.txt, native/JUCE/LICENSE.md,
vendor/basic-pitch-model/LICENSE-basic-pitch-apache-2.0.txt, and the
staged node.exe / node_modules themselves) -- not a template. Regenerate
it if any dependency is added, removed, or upgraded. The four MT3 Model
Pack entries above were generated the same way, from the actual
downloaded embeddable Python package and the actual installed
distributions in native/BeatShoreDesktop/python_engine/
ml_env_mt3_trimmed. MT3-ModelPack-THIRD-PARTY-NOTICES.txt specifically
is regenerable via native/installer/scripts/gather_mt3_licenses.py (see
that script's own header for usage) rather than hand-typed; the other
three are single files copied verbatim from their own real source
(python/LICENSE.txt in the downloaded embeddable package;
torch-*.dist-info/licenses/LICENSE and
mt3_infer-*.dist-info/licenses/LICENSE in the trimmed venv).
