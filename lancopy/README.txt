WC3 co-op LAN bundle.

Client setup on Windows:
  1. Install Wing Commander III (GOG 30th Anniversary Edition / Kilrathi Saga
     Windows build). Verify WC3W.EXE SHA-256:
       certutil -hashfile WC3W.EXE SHA256
     Expected: 1fffd1840356ea09b4e475c6648eda98e9485e1e421336dc927a25db274cd91f
  2. In the Kilrathi_Saga folder, rename XANLIB.DLL to XanLibReal.dll.
  3. Copy XanLib.dll and wc3net.ini from this folder into Kilrathi_Saga.
     Edit wc3net.ini so host= matches the host machine's LAN IPv4 address.
  4. Configure your joystick in the game's setup UI and launch WC3W.EXE.
  5. Restore vanilla any time by deleting XanLib.dll and renaming
     XanLibReal.dll back to XANLIB.DLL.

Both host and client must run the same build of XanLib.dll.
