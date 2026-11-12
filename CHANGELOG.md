# Changelog

## [0.6.0](https://github.com/jcsduncan/oscillate/compare/v0.5.1...v0.6.0) (2026-11-12)


### Features

* **audio_io,application,app:** direct ALSA backend with device reservation ([#388](https://github.com/jcsduncan/oscillate/issues/388)) ([e9a2b57](https://github.com/jcsduncan/oscillate/commit/e9a2b5787b6dc32a84420e5bae505a0f60b4a92e))


### Bug Fixes

* **tools:** retry dropped connections in the source-link check ([#389](https://github.com/jcsduncan/oscillate/issues/389)) ([03323a8](https://github.com/jcsduncan/oscillate/commit/03323a8dadc1c91bfb53d43c99793be90caf14b3))

## [0.5.1](https://github.com/jcsduncan/oscillate/compare/v0.5.0...v0.5.1) (2026-11-11)


### Performance Improvements

* **audio_io,engine,application,app,tools:** render playback ahead of the device so a slow block cannot click ([#385](https://github.com/jcsduncan/oscillate/issues/385)) ([60f9752](https://github.com/jcsduncan/oscillate/commit/60f9752668dbc9a4977c2a92694cb7f19648de18))
* **engine,application,track_chain,app,tools:** the master overlaps the tracks within each callback ([#382](https://github.com/jcsduncan/oscillate/issues/382)) ([91cb613](https://github.com/jcsduncan/oscillate/commit/91cb613b96949b24c1937531ae0bcea51b3e9e33))
* **native_dsp,tools:** cut the master chain's serial cost and gate it in CI ([#376](https://github.com/jcsduncan/oscillate/issues/376)) ([2d0ba59](https://github.com/jcsduncan/oscillate/commit/2d0ba59b58c684ee69193892a01fa990c6936ec7))
* **native_dsp,warp_dsp:** one realtime float FFT for the audio thread, Temper five times faster ([#381](https://github.com/jcsduncan/oscillate/issues/381)) ([cacbd2c](https://github.com/jcsduncan/oscillate/commit/cacbd2cffdef962b8cfd175e200d6c54e1a80798))

## [0.5.0](https://github.com/jcsduncan/oscillate/compare/v0.4.0...v0.5.0) (2026-11-10)


### Features

* **app:** Master Assistant panel on the master ([#374](https://github.com/jcsduncan/oscillate/issues/374)) ([e98eebb](https://github.com/jcsduncan/oscillate/commit/e98eebbfe24a97c4eb94805f506ed033915eca35))
* **app:** Rebalance panel on the master ([#372](https://github.com/jcsduncan/oscillate/issues/372)) ([dd873f4](https://github.com/jcsduncan/oscillate/commit/dd873f42128127b01a08226e1738ed37d5adfa50))
* **native_dsp,app,tools:** Gild multiband exciter on Forge's curves ([#363](https://github.com/jcsduncan/oscillate/issues/363)) ([4a92cc5](https://github.com/jcsduncan/oscillate/commit/4a92cc5b6a187dfb96368ad1a4903670b75e56dc))
* **native_dsp,app,tools:** Naked Drums as Stick's default kit ([#359](https://github.com/jcsduncan/oscillate/issues/359)) ([872cf51](https://github.com/jcsduncan/oscillate/commit/872cf51d2dea901917832da6ad1a23888311e392))
* **native_dsp,app,tools:** Stick's production chain and shaped presets ([#371](https://github.com/jcsduncan/oscillate/issues/371)) ([5aedf6a](https://github.com/jcsduncan/oscillate/commit/5aedf6aae24e2d0a414574575de0016b0e2886ec))
* **native_dsp,application,tools:** master assistant proposes a master chain from a reference and a loudness target ([#373](https://github.com/jcsduncan/oscillate/issues/373)) ([27f1f76](https://github.com/jcsduncan/oscillate/commit/27f1f76742b3378a69b3fb83884364cb889f1fb7))
* **project,engine,render_graph,application,analysis,persistence:** master rebalance by role from the project's tracks ([#362](https://github.com/jcsduncan/oscillate/issues/362)) ([81503c6](https://github.com/jcsduncan/oscillate/commit/81503c64046ecaa036f0781e487d7c7e3c6bb9b3))


### Bug Fixes

* **persistence,app:** open projects from every tagged release (1.47 → 1.48 migration) ([#370](https://github.com/jcsduncan/oscillate/issues/370)) ([499cfdf](https://github.com/jcsduncan/oscillate/commit/499cfdff9caa2031a6d6f01b5bd1d31bb4e35889))

## [0.4.0](https://github.com/jcsduncan/oscillate/compare/v0.3.0...v0.4.0) (2026-11-10)


### Features

* **app:** retire the automation forms; the arrangement lanes edit everything ([#336](https://github.com/jcsduncan/oscillate/issues/336)) ([8336289](https://github.com/jcsduncan/oscillate/commit/83362891049598f690a732fabec1965d1132a338))
* **app:** warp markers on the arrangement and the project tempo lane ([#330](https://github.com/jcsduncan/oscillate/issues/330)) ([a41fd83](https://github.com/jcsduncan/oscillate/commit/a41fd83b1990e8ace429f4f26d182498fe30f17f))
* **native_dsp,app,tools:** Keel adaptive tonal balance toward a target curve ([#356](https://github.com/jcsduncan/oscillate/issues/356)) ([de923e8](https://github.com/jcsduncan/oscillate/commit/de923e8b835b631c44af694aff63cefa57c80056))
* **native_dsp,app,tools:** match EQ and tonal balance targets in Lens ([#355](https://github.com/jcsduncan/oscillate/issues/355)) ([758069f](https://github.com/jcsduncan/oscillate/commit/758069ffcccf5083bf1cec3aaeaa60b9476ddfc7))
* **native_dsp,app,tools:** Stick sampled acoustic drums ([#342](https://github.com/jcsduncan/oscillate/issues/342)) ([fc551ef](https://github.com/jcsduncan/oscillate/commit/fc551ef31bb38b21e8d48a2a5e4afc710abeb09e))
* **native_dsp,app,tools:** Strike transient shaper, single-band and multiband ([#353](https://github.com/jcsduncan/oscillate/issues/353)) ([f611981](https://github.com/jcsduncan/oscillate/commit/f611981eeb79fdb40cbc6b7c0b41330642539ed7))
* **native_dsp,application,app:** Edge ceiling; linked effect edits all or nothing ([#340](https://github.com/jcsduncan/oscillate/issues/340)) ([7803e43](https://github.com/jcsduncan/oscillate/commit/7803e43fbb873c54c2d48202f8b42ab437cf6c19))
* **native_dsp,export,app,tools:** Ceiling mastering limiter and noise-shaped dither ([#352](https://github.com/jcsduncan/oscillate/issues/352)) ([7913d1c](https://github.com/jcsduncan/oscillate/commit/7913d1c9bc09c297de4f8271afa252aa3625e64e))
* **native_dsp,persistence,app:** Cast convolution reverb and Hush gate ([#351](https://github.com/jcsduncan/oscillate/issues/351)) ([812b2da](https://github.com/jcsduncan/oscillate/commit/812b2da1c2cfc30898242fc8dae0baa0cdb3f119))
* **tools,native_dsp:** breakdown, keys and strings audition modes, and the Rolling Sixteenth Bass preset ([#339](https://github.com/jcsduncan/oscillate/issues/339)) ([c2aa11e](https://github.com/jcsduncan/oscillate/commit/c2aa11e422316660928b77ef4c9a0e1845e9e537))
* **tools:** drums audition mode renders Stick grooves per preset ([#354](https://github.com/jcsduncan/oscillate/issues/354)) ([d0dd59f](https://github.com/jcsduncan/oscillate/commit/d0dd59faf9738b8cb0e6275404cba764950c20ec))


### Bug Fixes

* **app:** install live audio edits on the UI thread ([#334](https://github.com/jcsduncan/oscillate/issues/334)) ([5f9de12](https://github.com/jcsduncan/oscillate/commit/5f9de1253680f92f57c10e4934bd3fd64aaad656))

## [0.3.0](https://github.com/jcsduncan/oscillate/compare/v0.2.0...v0.3.0) (2026-11-09)


### Features

* **app,application,engine,project:** trim a MIDI clip from its left edge ([#275](https://github.com/jcsduncan/oscillate/issues/275)) ([704de78](https://github.com/jcsduncan/oscillate/commit/704de78556ee22288a208128a4c3551f5d4b88a1))
* **app,application:** Ctrl+drag a clip to copy it where you drop it ([#276](https://github.com/jcsduncan/oscillate/issues/276)) ([5a67b0e](https://github.com/jcsduncan/oscillate/commit/5a67b0e37f55695c864b501f6bbc45ad43021005))
* **app,application:** nest group buses in the track list and route by drag ([#274](https://github.com/jcsduncan/oscillate/issues/274)) ([a3882de](https://github.com/jcsduncan/oscillate/commit/a3882de28046572510c90572433678a4d290bb20))
* **app,native_dsp:** Edge, a clipper under a 0 dBTP ceiling ([#307](https://github.com/jcsduncan/oscillate/issues/307)) ([e056726](https://github.com/jcsduncan/oscillate/commit/e05672688431c6bbf576e89d767ef3865a9e10a8))
* **app,native_dsp:** the Bow panel, and one missing-library notice for every sampled instrument ([#322](https://github.com/jcsduncan/oscillate/issues/322)) ([a4ffbe1](https://github.com/jcsduncan/oscillate/commit/a4ffbe1b443b6ae5e8185dfcaab7aaecf245e338))
* **app:** dB scale on mixer faders, automated-track mark, browser opens on Instruments ([#302](https://github.com/jcsduncan/oscillate/issues/302)) ([732fde2](https://github.com/jcsduncan/oscillate/commit/732fde28d6a82af7ff7f0789e20cbdee156bc4d7))
* **app:** every instrument and effect panel on the shared device kit ([#320](https://github.com/jcsduncan/oscillate/issues/320)) ([c476787](https://github.com/jcsduncan/oscillate/commit/c476787840762beac92f87f02c91b364d2631c79))
* **application,track_chain,native_dsp:** switch VST3 instrument and tempo edits live ([#295](https://github.com/jcsduncan/oscillate/issues/295)) ([a08e117](https://github.com/jcsduncan/oscillate/commit/a08e1171f9d348b36077d93d3e2953df5b58d3d2))
* **application:** warp marker commands and tempo ramps for the arrangement ([#329](https://github.com/jcsduncan/oscillate/issues/329)) ([4802581](https://github.com/jcsduncan/oscillate/commit/4802581995832ed061e431ec43f74fd964b3553e))
* **app:** shared device header, status badge, control states, plot frame, panel zoom and keyboard reach ([#315](https://github.com/jcsduncan/oscillate/issues/315)) ([0547495](https://github.com/jcsduncan/oscillate/commit/0547495c25e875312c85ceba0cbf648692295ba1))
* **engine,application,native_dsp,native_inserts,app:** switch track, tempo, media and instrument edits live ([#287](https://github.com/jcsduncan/oscillate/issues/287)) ([16fa989](https://github.com/jcsduncan/oscillate/commit/16fa989a7e2696f842c2a45eeb740d412a3cb426))
* **native_dsp,tools:** Bow, sampled strings on the sample core ([#321](https://github.com/jcsduncan/oscillate/issues/321)) ([2f847ff](https://github.com/jcsduncan/oscillate/commit/2f847ff13e94aa349254e6250639521141e028df))
* **persistence,application,app:** keep pre-upgrade versions, save a copy and forget project caches ([#286](https://github.com/jcsduncan/oscillate/issues/286)) ([2b6e37c](https://github.com/jcsduncan/oscillate/commit/2b6e37c92dddb82e4ff94b913d37bba25365d694))
* **persistence:** open released project schemas through chained forward migrations ([#266](https://github.com/jcsduncan/oscillate/issues/266)) ([cffa9e0](https://github.com/jcsduncan/oscillate/commit/cffa9e0204bb649365460e38bb3399b5ad25d878))


### Bug Fixes

* **app:** fit Halo's keytrack knob label ([#256](https://github.com/jcsduncan/oscillate/issues/256)) ([edab48b](https://github.com/jcsduncan/oscillate/commit/edab48b50041bd9deda94a4f238a64fe418a3a3b))
* **app:** fit the device panels' text under CI's font ([#282](https://github.com/jcsduncan/oscillate/issues/282)) ([2e04a29](https://github.com/jcsduncan/oscillate/commit/2e04a291bbd0570a00c03ac359492348924b1ba9))
* **application,routing:** silence a bounced source under a mute lane ([#288](https://github.com/jcsduncan/oscillate/issues/288)) ([8aa5155](https://github.com/jcsduncan/oscillate/commit/8aa5155c3b9291f57cec2446ef4f5bd0f4cf462e))
* **app:** stop a project change when a plug-in editor's state is not saved ([#301](https://github.com/jcsduncan/oscillate/issues/301)) ([fc6fe73](https://github.com/jcsduncan/oscillate/commit/fc6fe734b094ae19efd1683366c63c6ee73313ca))
* **audio_io:** make five CI-flaky tests deterministic ([#290](https://github.com/jcsduncan/oscillate/issues/290)) ([a326d24](https://github.com/jcsduncan/oscillate/commit/a326d242058d411c3aa7c296ce80ddf103668211))
* **codec:** decode MP3 with libmpg123 so headerless VBR files import whole ([#304](https://github.com/jcsduncan/oscillate/issues/304)) ([bfb982a](https://github.com/jcsduncan/oscillate/commit/bfb982a68cf856bf23f784f3431d64ff2d322ee4))
* **engine,application,app:** switch sources at a primed block and remove the held path ([#273](https://github.com/jcsduncan/oscillate/issues/273)) ([2b587b4](https://github.com/jcsduncan/oscillate/commit/2b587b40e18363205b475ad7bcd07c0abda70d20))
* **engine,native_dsp,application:** switch MIDI edits live and queue rapid edits ([#261](https://github.com/jcsduncan/oscillate/issues/261)) ([95ffecc](https://github.com/jcsduncan/oscillate/commit/95ffecc01f0b17687648c9679cf16215e9060167))
* **project,export,application:** keep a clip envelope's precedence through a bounce or freeze ([#293](https://github.com/jcsduncan/oscillate/issues/293)) ([27b683c](https://github.com/jcsduncan/oscillate/commit/27b683cc224f393318564f95c119c5fd4ef1ae05))
* **routing,native_inserts:** silence a key wherever its source's mute lane mutes it ([#267](https://github.com/jcsduncan/oscillate/issues/267)) ([bddb3c0](https://github.com/jcsduncan/oscillate/commit/bddb3c0639065b048bf984ebc2c12454dd043e29))


### Performance Improvements

* **engine,native_dsp:** spread heavy projects over the audio helpers and cut Forge's cost ([#319](https://github.com/jcsduncan/oscillate/issues/319)) ([f46ec9e](https://github.com/jcsduncan/oscillate/commit/f46ec9ebfcf120ffdf8660d088bccd6f1b191296))

## [0.2.0](https://github.com/jcsduncan/oscillate/compare/oscillate-v0.1.0...oscillate-v0.2.0) (2026-11-08)


### Features

* **app,application:** select every arrangement clip with Ctrl+A and move the selection together ([#244](https://github.com/jcsduncan/oscillate/issues/244)) ([e1ab848](https://github.com/jcsduncan/oscillate/commit/e1ab84863a72781d31c5512708bf002a219141c6))
* **app,foundation:** local crash reports and a published known-issues page ([#196](https://github.com/jcsduncan/oscillate/issues/196)) ([6e28c3c](https://github.com/jcsduncan/oscillate/commit/6e28c3c155d8397469fdf89c26d5006801430cc4))
* **app,foundation:** save a diagnostics bundle from the command palette or --save-diagnostics ([#209](https://github.com/jcsduncan/oscillate/issues/209)) ([ed90019](https://github.com/jcsduncan/oscillate/commit/ed90019a2270406122b05b4fc85bb15934bd779a))
* **application:** forget a project in the cache registry ([#158](https://github.com/jcsduncan/oscillate/issues/158)) ([d55290f](https://github.com/jcsduncan/oscillate/commit/d55290fdcbb6872f672052583688eeade24a8b15))
* **application:** session services and playback control (1/9) ([#32](https://github.com/jcsduncan/oscillate/issues/32)) ([5c70195](https://github.com/jcsduncan/oscillate/commit/5c70195bc70f604e0d394e7f711a6286fc2368ed))
* **application:** session services and playback control (2/9) ([#33](https://github.com/jcsduncan/oscillate/issues/33)) ([4b79fb1](https://github.com/jcsduncan/oscillate/commit/4b79fb15484cc5f799301fb1ecdb0e0a59477991))
* **application:** session services and playback control (3/9) ([#34](https://github.com/jcsduncan/oscillate/issues/34)) ([6d4f3de](https://github.com/jcsduncan/oscillate/commit/6d4f3ded56004106d8294f33088838339bcc892f))
* **application:** session services and playback control (4/9) ([#37](https://github.com/jcsduncan/oscillate/issues/37)) ([7344a3e](https://github.com/jcsduncan/oscillate/commit/7344a3e1d3119cf73d52341e9147a24cdf7d4532))
* **application:** session services and playback control (5/9) ([#38](https://github.com/jcsduncan/oscillate/issues/38)) ([1c055a6](https://github.com/jcsduncan/oscillate/commit/1c055a6b69da1e2627da9a837e375b86ce7c22a5))
* **application:** session services and playback control (6/9) ([#41](https://github.com/jcsduncan/oscillate/issues/41)) ([76c7e1d](https://github.com/jcsduncan/oscillate/commit/76c7e1dbbdd2ba828624282b5a79624560a2b2fa))
* **application:** session services and playback control (7/9) ([#42](https://github.com/jcsduncan/oscillate/issues/42)) ([e29b074](https://github.com/jcsduncan/oscillate/commit/e29b0742be4ab3cceaca0be8f7045567d4f30f5a))
* **application:** session services and playback control (8/9) ([#50](https://github.com/jcsduncan/oscillate/issues/50)) ([5f44ee0](https://github.com/jcsduncan/oscillate/commit/5f44ee0a1e9f9c7a079fbb41ac51303d942901e7))
* **application:** session services and playback control (9/9) ([#55](https://github.com/jcsduncan/oscillate/issues/55)) ([3c2c115](https://github.com/jcsduncan/oscillate/commit/3c2c1156c2af01b9e9ec39ff3610336994d20ddd))
* **app:** live device-chain strip views for Ceiling, Press, Trim, Lens, Scope and Loudness ([#159](https://github.com/jcsduncan/oscillate/issues/159)) ([6041217](https://github.com/jcsduncan/oscillate/commit/604121783c3b0cfcd75503feb01775e7b4588e15))
* **app:** Qt Quick application and its tests (1/22) ([#51](https://github.com/jcsduncan/oscillate/issues/51)) ([c87b0f0](https://github.com/jcsduncan/oscillate/commit/c87b0f030465c263f3c8837e30734c1c97ae6150))
* **app:** Qt Quick application and its tests (10/22) ([#61](https://github.com/jcsduncan/oscillate/issues/61)) ([bfca60e](https://github.com/jcsduncan/oscillate/commit/bfca60e50c1399a8bf508511ea940db16ac3d25f))
* **app:** Qt Quick application and its tests (11/22) ([#62](https://github.com/jcsduncan/oscillate/issues/62)) ([9075839](https://github.com/jcsduncan/oscillate/commit/907583989fbffde64f7f7377204705e7b6bf7056))
* **app:** Qt Quick application and its tests (12/22) ([#63](https://github.com/jcsduncan/oscillate/issues/63)) ([7b8ce70](https://github.com/jcsduncan/oscillate/commit/7b8ce7071178aba529c918d40abb78f3ee5b902e))
* **app:** Qt Quick application and its tests (13/22) ([#64](https://github.com/jcsduncan/oscillate/issues/64)) ([2c03a8f](https://github.com/jcsduncan/oscillate/commit/2c03a8f3b7fd2be8dd62f3a23b3fa169851cab27))
* **app:** Qt Quick application and its tests (14/22) ([#66](https://github.com/jcsduncan/oscillate/issues/66)) ([31eb43e](https://github.com/jcsduncan/oscillate/commit/31eb43edc357b0a1d33410241bf29a4098e93618))
* **app:** Qt Quick application and its tests (15/22) ([#67](https://github.com/jcsduncan/oscillate/issues/67)) ([655c03e](https://github.com/jcsduncan/oscillate/commit/655c03e726248a024e107f69ef500d2a3fefdf0b))
* **app:** Qt Quick application and its tests (16/22) ([#69](https://github.com/jcsduncan/oscillate/issues/69)) ([b70139e](https://github.com/jcsduncan/oscillate/commit/b70139e6be0747a99372883964d0fd36cdea4e73))
* **app:** Qt Quick application and its tests (17/22) ([#70](https://github.com/jcsduncan/oscillate/issues/70)) ([21f43d9](https://github.com/jcsduncan/oscillate/commit/21f43d93875b21e748eec7430ed87de6a33132c6))
* **app:** Qt Quick application and its tests (18/22) ([#71](https://github.com/jcsduncan/oscillate/issues/71)) ([46c76d7](https://github.com/jcsduncan/oscillate/commit/46c76d74024ed44e03792ab80901416520763ebd))
* **app:** Qt Quick application and its tests (19/22) ([#72](https://github.com/jcsduncan/oscillate/issues/72)) ([a8f91c4](https://github.com/jcsduncan/oscillate/commit/a8f91c4f129d179692b41213d73f58abee88c55d))
* **app:** Qt Quick application and its tests (2/22) ([#52](https://github.com/jcsduncan/oscillate/issues/52)) ([cb24616](https://github.com/jcsduncan/oscillate/commit/cb24616ed2920daabc3ca8718836447ab52a50db))
* **app:** Qt Quick application and its tests (20/22) ([#73](https://github.com/jcsduncan/oscillate/issues/73)) ([d7e7024](https://github.com/jcsduncan/oscillate/commit/d7e7024be42d722017e28dea1122d5eed9045d4d))
* **app:** Qt Quick application and its tests (21/22) ([#74](https://github.com/jcsduncan/oscillate/issues/74)) ([a65ff28](https://github.com/jcsduncan/oscillate/commit/a65ff28d230fc5c13c1021ea50c065f2da32db55))
* **app:** Qt Quick application and its tests (22/22) ([#79](https://github.com/jcsduncan/oscillate/issues/79)) ([43d8e4c](https://github.com/jcsduncan/oscillate/commit/43d8e4c27e9d6a92be8a4e2f49e61bb9cf24c1a2))
* **app:** Qt Quick application and its tests (3/22) ([#53](https://github.com/jcsduncan/oscillate/issues/53)) ([88af3cb](https://github.com/jcsduncan/oscillate/commit/88af3cbcb9b83e1251bb4693e9d35bc4c86487b6))
* **app:** Qt Quick application and its tests (4/22) ([#54](https://github.com/jcsduncan/oscillate/issues/54)) ([67fd70d](https://github.com/jcsduncan/oscillate/commit/67fd70d3be56f70de1294e66aa622a94155ea686))
* **app:** Qt Quick application and its tests (5/22) ([#56](https://github.com/jcsduncan/oscillate/issues/56)) ([6c5f456](https://github.com/jcsduncan/oscillate/commit/6c5f456745bea691ff6c74fd86e12f0fa018c34a))
* **app:** Qt Quick application and its tests (6/22) ([#57](https://github.com/jcsduncan/oscillate/issues/57)) ([64e54f6](https://github.com/jcsduncan/oscillate/commit/64e54f61666b27db9791aae599ebff42886bcc71))
* **app:** Qt Quick application and its tests (7/22) ([#58](https://github.com/jcsduncan/oscillate/issues/58)) ([d8b01a1](https://github.com/jcsduncan/oscillate/commit/d8b01a198d91277919727919fe4fb0f50f8eca36))
* **app:** Qt Quick application and its tests (8/22) ([#59](https://github.com/jcsduncan/oscillate/issues/59)) ([908ccdf](https://github.com/jcsduncan/oscillate/commit/908ccdfd438f1b66ab4d294b7e81bf4acb467a0e))
* **app:** Qt Quick application and its tests (9/22) ([#60](https://github.com/jcsduncan/oscillate/issues/60)) ([5318e11](https://github.com/jcsduncan/oscillate/commit/5318e112be105ac8a2914edb863e43ad0fdd560f))
* **audio_io:** PipeWire and JACK output ([#23](https://github.com/jcsduncan/oscillate/issues/23)) ([9ff1f63](https://github.com/jcsduncan/oscillate/commit/9ff1f633dd9c1275991852e52eb042101159419b))
* bootstrap and foundation ([#2](https://github.com/jcsduncan/oscillate/issues/2)) ([c9057d7](https://github.com/jcsduncan/oscillate/commit/c9057d7de0f0972feb3473e9186e6fb0c4d4daa8))
* **clay_dsp:** note analysis and resynthesis ([#30](https://github.com/jcsduncan/oscillate/issues/30)) ([62bddd6](https://github.com/jcsduncan/oscillate/commit/62bddd6a968dc221b05e1334dfae588c1bb9f720))
* codec, midi file and automation project ([#6](https://github.com/jcsduncan/oscillate/issues/6)) ([45082c6](https://github.com/jcsduncan/oscillate/commit/45082c666beaeeb75c55bb3af90373fc282b672c))
* **engine:** audio graph, events and real-time services (1/2) ([#8](https://github.com/jcsduncan/oscillate/issues/8)) ([fd0f5e1](https://github.com/jcsduncan/oscillate/commit/fd0f5e142ed0b8adcec470b56b849e3310934958))
* **engine:** audio graph, events and real-time services (2/2) ([#12](https://github.com/jcsduncan/oscillate/issues/12)) ([fbe9074](https://github.com/jcsduncan/oscillate/commit/fbe907413e0cff5173f9d6f659d17a84b06cc40e))
* **export:** offline and real-time export ([#46](https://github.com/jcsduncan/oscillate/issues/46)) ([01bffc7](https://github.com/jcsduncan/oscillate/commit/01bffc7de17e2f46fbf68631e134e7d7ab9f5327))
* **harmony:** chords, voicing and the harmony lab ([#17](https://github.com/jcsduncan/oscillate/issues/17)) ([e8eeb0b](https://github.com/jcsduncan/oscillate/commit/e8eeb0bd366a9f0c8203687d0371c5999ada66c4))
* **native_dsp,tools:** add Nova pads for the flute: Santur Bloom, Ney Breath, Warm Sub Bed ([#234](https://github.com/jcsduncan/oscillate/issues/234)) ([e35a2de](https://github.com/jcsduncan/oscillate/commit/e35a2de2e975e9af4768aa16a5d1d022cbf35775))
* **native_dsp:** native synths, effects and DSP (1/8) ([#14](https://github.com/jcsduncan/oscillate/issues/14)) ([97eb7e6](https://github.com/jcsduncan/oscillate/commit/97eb7e663499e0e99f913b7818c38a4ae452715d))
* **native_dsp:** native synths, effects and DSP (2/8) ([#15](https://github.com/jcsduncan/oscillate/issues/15)) ([47b8bcf](https://github.com/jcsduncan/oscillate/commit/47b8bcf0bb82c4eb438fd7dcc43e5fc9093263a9))
* **native_dsp:** native synths, effects and DSP (3/8) ([#16](https://github.com/jcsduncan/oscillate/issues/16)) ([04777a4](https://github.com/jcsduncan/oscillate/commit/04777a457e2a78d071ff2b746441b31221121f59))
* **native_dsp:** native synths, effects and DSP (4/8) ([#18](https://github.com/jcsduncan/oscillate/issues/18)) ([25e08e4](https://github.com/jcsduncan/oscillate/commit/25e08e4736ec00c64e892f40bd68a5ea32196e2e))
* **native_dsp:** native synths, effects and DSP (5/8) ([#19](https://github.com/jcsduncan/oscillate/issues/19)) ([b913ef9](https://github.com/jcsduncan/oscillate/commit/b913ef9167108694ab1b9a3d210bb171b38c07c3))
* **native_dsp:** native synths, effects and DSP (6/8) ([#21](https://github.com/jcsduncan/oscillate/issues/21)) ([61a6c63](https://github.com/jcsduncan/oscillate/commit/61a6c63389a0880f72a46a7a661bcc0e150ecc37))
* **native_dsp:** native synths, effects and DSP (7/8) ([#24](https://github.com/jcsduncan/oscillate/issues/24)) ([e43bdb1](https://github.com/jcsduncan/oscillate/commit/e43bdb1ed74d31a5408dd1a670778eb24efeec61))
* **native_dsp:** native synths, effects and DSP (8/8) ([#36](https://github.com/jcsduncan/oscillate/issues/36)) ([baafab2](https://github.com/jcsduncan/oscillate/commit/baafab27d8971a35407feead8e1b4fec0043b598))
* **persistence:** project store and codec (1/2) ([#13](https://github.com/jcsduncan/oscillate/issues/13)) ([dc82568](https://github.com/jcsduncan/oscillate/commit/dc8256879c5f1dcf745850552fefa0226bc74110))
* **persistence:** project store and codec (2/2) ([#28](https://github.com/jcsduncan/oscillate/issues/28)) ([0a7fc3c](https://github.com/jcsduncan/oscillate/commit/0a7fc3ce659a95a8c94ff70ec54284e6bdb5077e))
* plugin ui and integration ([#65](https://github.com/jcsduncan/oscillate/issues/65)) ([51a040a](https://github.com/jcsduncan/oscillate/commit/51a040a960513950674f6a790dc47331ad0d89dd))
* **plugins:** VST3 hosting (1/2) ([#31](https://github.com/jcsduncan/oscillate/issues/31)) ([b803b94](https://github.com/jcsduncan/oscillate/commit/b803b94c212410161c0df1d65e8aa8cad1cbcc32))
* **plugins:** VST3 hosting (2/2) ([#39](https://github.com/jcsduncan/oscillate/issues/39)) ([8305424](https://github.com/jcsduncan/oscillate/commit/8305424f9e696a34cc58ed187cebf0dd7f0afa77))
* processing and source prep ([#35](https://github.com/jcsduncan/oscillate/issues/35)) ([cbe2774](https://github.com/jcsduncan/oscillate/commit/cbe27740370513135a9bf290568a7183a20b4955))
* **project:** bound the undo history by depth and estimated bytes ([#183](https://github.com/jcsduncan/oscillate/issues/183)) ([b640485](https://github.com/jcsduncan/oscillate/commit/b640485a2c44f7b5bd81f6e29616b14eb89dfa83))
* **project:** project model, commands and validation ([#4](https://github.com/jcsduncan/oscillate/issues/4)) ([7e5161a](https://github.com/jcsduncan/oscillate/commit/7e5161a42bb900b2f83e189b8eb9c805a08ad292))
* routing and media ([#22](https://github.com/jcsduncan/oscillate/issues/22)) ([af45b30](https://github.com/jcsduncan/oscillate/commit/af45b30501341050a3649c6671f5b4cc36b295a7))
* timeline and automation ([#3](https://github.com/jcsduncan/oscillate/issues/3)) ([a5a00f1](https://github.com/jcsduncan/oscillate/commit/a5a00f12844715cff34d59476cc7624853ccd957))
* **tools:** benches, listening tools, installers, scripts and Python CI (1/2) ([#75](https://github.com/jcsduncan/oscillate/issues/75)) ([fb80364](https://github.com/jcsduncan/oscillate/commit/fb803648705bdbc329268f1e34b794d91392fae7))
* **tools:** benches, listening tools, installers, scripts and Python CI (2/2) ([#80](https://github.com/jcsduncan/oscillate/issues/80)) ([50995de](https://github.com/jcsduncan/oscillate/commit/50995de980498146305d6d3aadc3eaca16602f5d))
* track chain and analysis ([#43](https://github.com/jcsduncan/oscillate/issues/43)) ([e034098](https://github.com/jcsduncan/oscillate/commit/e034098f903affe8c1c768db886997bbaa6944c9))
* **warp_dsp:** time-stretch and pitch engines ([#25](https://github.com/jcsduncan/oscillate/issues/25)) ([8897f6c](https://github.com/jcsduncan/oscillate/commit/8897f6c5c99f1546862a822b1742c676ffa58744))


### Bug Fixes

* **app,application:** drop job results for a replaced session ([#179](https://github.com/jcsduncan/oscillate/issues/179)) ([6a09249](https://github.com/jcsduncan/oscillate/commit/6a09249e2614bfa7bf3aa6a905616c1289f5288f))
* **app,foundation,native_dsp:** one value formatter for every panel ([#222](https://github.com/jcsduncan/oscillate/issues/222)) ([67c608e](https://github.com/jcsduncan/oscillate/commit/67c608e962b3acf0c86353c84a4d1fa2c4ac243b))
* **app,plugins:** hash the VST3 editor's binary on a worker ([#192](https://github.com/jcsduncan/oscillate/issues/192)) ([28d7757](https://github.com/jcsduncan/oscillate/commit/28d7757abe0bf2bcc6edbcd1570f155943447e2d))
* **app:** audition releases its oldest note when every held slot is taken ([#148](https://github.com/jcsduncan/oscillate/issues/148)) ([dbde5fb](https://github.com/jcsduncan/oscillate/commit/dbde5fba98a5f27ac3d39d91e87c6899627b50af))
* **app:** chain strip draws the output spectrum, not the raw telemetry frame ([#127](https://github.com/jcsduncan/oscillate/issues/127)) ([a1ff9d7](https://github.com/jcsduncan/oscillate/commit/a1ff9d7555bf03624ccc7b28f73373374b83b479))
* **app:** drop key analysis for a session that was replaced ([#182](https://github.com/jcsduncan/oscillate/issues/182)) ([cce5841](https://github.com/jcsduncan/oscillate/commit/cce5841e97bd465304ac9b154f76cd4d3b055213))
* **app:** keep the UI off the routed run while it stops ([#178](https://github.com/jcsduncan/oscillate/issues/178)) ([a00a95d](https://github.com/jcsduncan/oscillate/commit/a00a95d785b03fdc93a28bb9e9c5679aa5f813e8))
* **application,engine:** withdraw a live switch that can never take over ([#175](https://github.com/jcsduncan/oscillate/issues/175)) ([ed7aff3](https://github.com/jcsduncan/oscillate/commit/ed7aff3b32464a8e4a7a6faebf79fb1b9657eda3))
* **application,harmony,clay_dsp,processing,native_inserts,app:** clear clang-tidy findings in the application layer and app controllers ([#242](https://github.com/jcsduncan/oscillate/issues/242)) ([f5fee18](https://github.com/jcsduncan/oscillate/commit/f5fee187c94057e5be924b4cc68654acb68d0680))
* **application:** bound Clay follow targets to the repeats under the clip ([#153](https://github.com/jcsduncan/oscillate/issues/153)) ([8c7ef07](https://github.com/jcsduncan/oscillate/commit/8c7ef07345507a86b606ac6dcab280a8e3697a4e))
* **application:** bound MIDI consolidation expansion ([#154](https://github.com/jcsduncan/oscillate/issues/154)) ([879c160](https://github.com/jcsduncan/oscillate/commit/879c16027e612ab24e15326cfc1cc9a9b1a2cd91))
* **app:** never wait for a cancelled Clay detection or render ([#194](https://github.com/jcsduncan/oscillate/issues/194)) ([9970b97](https://github.com/jcsduncan/oscillate/commit/9970b979dd4ee9759cba41322320e2548aa033c1))
* **app:** one value per parameter and offset when a VST3 manual edit meets a pedal ([#155](https://github.com/jcsduncan/oscillate/issues/155)) ([cd0cf12](https://github.com/jcsduncan/oscillate/commit/cd0cf1223aee9ed64155b5846e34517435266357))
* **app:** read MIDI and groove files on a worker ([#189](https://github.com/jcsduncan/oscillate/issues/189)) ([792b50d](https://github.com/jcsduncan/oscillate/commit/792b50dede60e916161cd8a2821f04e0b4706190))
* **app:** readable hint text: 4.5:1 faint ink and a 9 px type floor ([#186](https://github.com/jcsduncan/oscillate/issues/186)) ([bdd4952](https://github.com/jcsduncan/oscillate/commit/bdd4952b01ca3c5e8557e41899deb71f5853074b))
* **app:** save a performance trace on a worker ([#185](https://github.com/jcsduncan/oscillate/issues/185)) ([e24ec7c](https://github.com/jcsduncan/oscillate/commit/e24ec7c8476e5511df83a5a46d4ed8aeff3316a5))
* **app:** segmented controls fill the selected option with the accent ([#216](https://github.com/jcsduncan/oscillate/issues/216)) ([2432f9a](https://github.com/jcsduncan/oscillate/commit/2432f9a7a60b5c27ea4899bb90857d8388cdd20b))
* **app:** truncated, clipped and overlapping panel text ([#251](https://github.com/jcsduncan/oscillate/issues/251)) ([e745ad2](https://github.com/jcsduncan/oscillate/commit/e745ad2860384013f5a68d596eecf9f89abbbd84))
* **codec,application,app:** report a headerless MP3's decodable length and stop the keyless-drop revision loop ([#253](https://github.com/jcsduncan/oscillate/issues/253)) ([aa7cd52](https://github.com/jcsduncan/oscillate/commit/aa7cd523ae92bb9089e42c1345a6910ec7f320d3))
* **engine:** bound concurrent notes so every held note is released at a loop wrap ([#151](https://github.com/jcsduncan/oscillate/issues/151)) ([48c2bff](https://github.com/jcsduncan/oscillate/commit/48c2bffafc22ea01b3cdeb22e533d49a4995c6f5))
* **engine:** withdraw a pending mixer candidate through the callback ([#152](https://github.com/jcsduncan/oscillate/issues/152)) ([2f37b26](https://github.com/jcsduncan/oscillate/commit/2f37b269ca0816b4143bf74bbc9d29ee5fc456ff))
* **foundation,engine,analysis,plugins,warp_dsp:** clear clang-tidy findings in the core modules ([#220](https://github.com/jcsduncan/oscillate/issues/220)) ([4f2652f](https://github.com/jcsduncan/oscillate/commit/4f2652fb426a83fd83be7a73fe40bf0524cc8f9a))
* **native_dsp:** clear clang-tidy findings in the native DSP and synth engines ([#237](https://github.com/jcsduncan/oscillate/issues/237)) ([987f0d0](https://github.com/jcsduncan/oscillate/commit/987f0d076a797297b398b629ef05c492bb126713))
* **native_dsp:** Prism mono LFOs follow their rate routes ([#156](https://github.com/jcsduncan/oscillate/issues/156)) ([ddc58f1](https://github.com/jcsduncan/oscillate/commit/ddc58f1e1792202ad3a60881da1ea41c763c94d9))
* **persistence,application,app:** create projects in a staged folder and rename them into place ([#212](https://github.com/jcsduncan/oscillate/issues/212)) ([6cedf40](https://github.com/jcsduncan/oscillate/commit/6cedf4041843ef90e2b84905bd2044f56435a9a9))
* **project:** unlink an effect an edit leaves alone in its group ([#157](https://github.com/jcsduncan/oscillate/issues/157)) ([814ebb8](https://github.com/jcsduncan/oscillate/commit/814ebb8b6d146c461cf02313796df8a4bb17828b))
* **routing,engine,native_inserts,application:** key Pump only from a kick you can hear ([#246](https://github.com/jcsduncan/oscillate/issues/246)) ([25cce68](https://github.com/jcsduncan/oscillate/commit/25cce683a21ff5b979e81619f40b4eaf91f95475))
