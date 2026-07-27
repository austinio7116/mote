# SDL's Java glue and MoteActivity.moteVibrate are reached from native code, so
# nothing here may be renamed or stripped.
-keep class org.libsdl.app.** { *; }
-keep class us.thumby.mote.** { *; }
