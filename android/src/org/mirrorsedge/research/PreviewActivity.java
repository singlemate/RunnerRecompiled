package org.mirrorsedge.research;
import android.app.Activity;
import android.os.Bundle;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.view.WindowManager;
import android.view.*;
import android.hardware.input.InputManager;
import android.widget.*;
import java.net.*;
import java.io.*;
import java.nio.ByteBuffer;

/** Diagnostic CPU readback viewer, not a Surface/swapchain. */
public class PreviewActivity extends Activity {
 private volatile boolean active;
 private volatile Socket socket;
 private Thread worker;
 private ImageView image;
 private TextView status;
 private final GuestInput guestInput=new GuestInput();private LinearLayout inputBar;private InputManager inputs;
 private final InputManager.InputDeviceListener inputListener=new InputManager.InputDeviceListener(){
  public void onInputDeviceAdded(int id){updateDevices();}public void onInputDeviceChanged(int id){guestInput.clear();updateDevices();}public void onInputDeviceRemoved(int id){guestInput.clear();updateDevices();}
 };
 private void updateDevices(){boolean external=false;for(int id:InputDevice.getDeviceIds()){InputDevice d=InputDevice.getDevice(id);if(d==null||d.isVirtual())continue;int sources=d.getSources();external|=(sources&InputDevice.SOURCE_GAMEPAD)==InputDevice.SOURCE_GAMEPAD || (sources&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK || d.getKeyboardType()==InputDevice.KEYBOARD_TYPE_ALPHABETIC || (sources&InputDevice.SOURCE_MOUSE)==InputDevice.SOURCE_MOUSE;}if(inputBar!=null)inputBar.setVisibility(external?View.GONE:View.VISIBLE);}
 private void addInputButton(String text,int bit){Button button=new Button(this);button.setText(text);button.setOnTouchListener((v,e)->{if(e.getActionMasked()==MotionEvent.ACTION_DOWN){guestInput.button(bit,true,true);return true;}if(e.getActionMasked()==MotionEvent.ACTION_UP||e.getActionMasked()==MotionEvent.ACTION_CANCEL){guestInput.button(bit,false,true);return true;}return true;});inputBar.addView(button,new LinearLayout.LayoutParams(0,-2,1));}
 @Override public boolean dispatchKeyEvent(KeyEvent e){int bit=GuestInput.keyBit(e.getKeyCode());if(bit!=0){guestInput.button(bit,e.getAction()==KeyEvent.ACTION_DOWN,false);return true;}return super.dispatchKeyEvent(e);}
 @Override public boolean dispatchGenericMotionEvent(MotionEvent e){return guestInput.motion(e)||super.dispatchGenericMotionEvent(e);}
 @Override public void onWindowFocusChanged(boolean focus){super.onWindowFocusChanged(focus);if(!focus)guestInput.clear();}

 @Override public void onCreate(Bundle state){
  super.onCreate(state);getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
  LinearLayout root=new LinearLayout(this);root.setOrientation(1);root.setBackgroundColor(Color.BLACK);root.setFitsSystemWindows(true);
  status=new TextView(this);status.setTextColor(Color.WHITE);status.setText("Подключение к игровому runtime…");root.addView(status);
  image=new ImageView(this);image.setScaleType(ImageView.ScaleType.FIT_CENTER);root.addView(image,new LinearLayout.LayoutParams(-1,0,1));
  inputBar=new LinearLayout(this);inputBar.setOrientation(0);addInputButton("START",0x10);addInputButton("A",0x1000);addInputButton("B",0x2000);root.addView(inputBar);setContentView(root);
  inputs=(InputManager)getSystemService(INPUT_SERVICE);
  guestInput.player(LocalPlayer.load(this));

 }
 @Override public void onResume(){super.onResume();active=true;guestInput.start();inputs.registerInputDeviceListener(inputListener,null);updateDevices();worker=new Thread(this::receive,"guest-preview");worker.start();}
 @Override public void onPause(){guestInput.stop();inputs.unregisterInputDeviceListener(inputListener);active=false;Socket old=socket;if(old!=null)try{old.close();}catch(IOException ignored){}if(worker!=null)worker.interrupt();super.onPause();}
 private void message(String value){runOnUiThread(()->{if(active)status.setText(value);});}
 private void receive(){
  final Thread owner=Thread.currentThread();long count=0;
  while(active && worker==owner && !owner.isInterrupted()){
   try(Socket connection=new Socket()){
    socket=connection;connection.connect(new InetSocketAddress("127.0.0.1",38761),1500);connection.setSoTimeout(6000);
    DataInputStream data=new DataInputStream(new BufferedInputStream(connection.getInputStream()));
    while(active && worker==owner){
     int magic=data.readInt(),w=data.readInt(),h=data.readInt(),length=data.readInt();
     if(magic!=0x4D454631 || w<1 || h<1 || w>4096 || h>4096 || (long)w*h*4!=length)throw new IOException("Frame header");
     byte[] pixels=new byte[length];data.readFully(pixels);
     Bitmap frame=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);frame.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
     long number=++count;
     runOnUiThread(()->{if(active && worker==owner){image.setImageBitmap(frame);status.setText("Живой просмотр · кадр "+number+" · "+w+"×"+h+" · START: контроллер / Enter / экран");}});
    }
   }catch(IOException|IllegalArgumentException e){message("Ожидание игрового runtime · просмотр отключён");}
   finally{if(worker==owner)socket=null;}
   try{Thread.sleep(1000);}catch(InterruptedException e){return;}
  }
 }
}
