# bindscript-vhid

A Windows KMDF HID minidriver. It presents one virtual joystick, "Bindscript Panel", with 8 axes and 128 buttons.
A user-mode application writes HID output reports to the device to set the axes and buttons.
This is the virtual controller driver used by bindscript.

## Build

Install Visual Studio with the Windows Driver Kit. Run this command in the repository folder:

```
msbuild bindscript_vhid.vcxproj /p:Configuration=Release /p:Platform=x64
```

The build writes the .sys, .inf and .cat files to `x64\Release\bindscript_vhid\`.

## Warning

The hardware id uses VID 0x1209 and PID 0x0001. This is the pid.codes test PID.
The test PID is not unique. Do not use this driver outside test environments.

## Licence

MIT. Refer to `LICENSE`.
