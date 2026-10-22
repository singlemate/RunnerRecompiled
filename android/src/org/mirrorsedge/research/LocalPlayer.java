package org.mirrorsedge.research;
import android.content.Context;
import android.content.SharedPreferences;
import java.security.SecureRandom;
/** Persistent app-local identity. Never regenerated when an existing value is invalid. */
final class LocalPlayer {
 static long load(Context context){
  SharedPreferences p=context.getSharedPreferences("offline-player-v1",Context.MODE_PRIVATE);
  if(p.contains("xuid")){long id=p.getLong("xuid",0);if((id&0xffff000000000000L)!=0xe000000000000000L || (id&0x0000ffffffffffffL)==0)throw new IllegalStateException("Invalid local player identity");return id;}
  long id=0xe000000000000000L|(new SecureRandom().nextLong()&0x0000ffffffffffffL);if((id&0x0000ffffffffffffL)==0)id|=1;
  if(!p.edit().putLong("xuid",id).putString("name","Runner").putInt("version",1).commit())throw new IllegalStateException("Could not persist local player identity");return id;
 }
}
