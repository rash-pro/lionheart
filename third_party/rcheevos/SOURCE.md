# rcheevos

- Origin: https://github.com/RetroAchievements/rcheevos
- Commit: bbc79b58a5a2a0b98dfbf2be6ca9e42c764661ce
- License: MIT (see LICENSE)
- Vendored from the upstream source at this revision.
- Only runtime, hash, utility and JSON parser sources are compiled. No client,
  login, HTTP transport or unlock submission is used. Local changes remove an unused console hash and its autodetection entry;
the fallback array is compacted. An unused documentation URL is removed. Two file extension literals are split into adjacent C strings without changing their values.

Unused API/ROM/ZIP/encrypted hashing sources and project files are omitted.
The Unlicense AES files are not included. The retained MD5 implementation has
its original zlib-style license notice, in addition to the project MIT license.
