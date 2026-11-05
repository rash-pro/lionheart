# rcheevos

- Origin: https://github.com/RetroAchievements/rcheevos
- Commit: bbc79b58a5a2a0b98dfbf2be6ca9e42c764661ce
- License: MIT (see LICENSE)
- Vendored from the upstream source at this revision.
- Runtime, PlayStation disc hashing, utilities, the client and its user/runtime/info
  API sources are compiled. Lionheart supplies its own asynchronous HTTP transport.
  The client, API headers and version utility were added from the same revision;
  these additions are unmodified. No external client or RAIntegration is built.
  Local changes to the existing hash sources remove an unused console hash and its autodetection entry;
the fallback array is compacted. An unused documentation URL is removed. Two file extension literals are split into adjacent C strings without changing their values.

Unused editor API/ROM/ZIP/encrypted hashing sources and project files are omitted.
The Unlicense AES files are not included. The retained MD5 implementation has
its original zlib-style license notice, in addition to the project MIT license.
