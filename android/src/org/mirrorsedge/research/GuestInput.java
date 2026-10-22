package org.mirrorsedge.research;
import android.view.*;
import java.net.*;
import java.nio.*;
/** Loopback input for user zero. No profile or save service is implied. */
final class GuestInput {
 private volatile boolean active;private Thread sender;private int keys,touch,hat,sequence;private final short[] axes=new short[4];private int lt,rt;private long localPlayer;
 synchronized void player(long xuid){localPlayer=xuid;}
 synchronized void button(int bit,boolean down,boolean virtual){if(virtual)touch=down?touch|bit:touch&~bit;else keys=down?keys|bit:keys&~bit;}
 synchronized void clear(){keys=touch=hat=lt=rt=0;java.util.Arrays.fill(axes,(short)0);}
 void start(){active=true;Thread owner=new Thread(this::sendLoop,"guest-input");sender=owner;owner.start();}
 void stop(){active=false;clear();Thread old=sender;if(old!=null)old.interrupt();}
 private synchronized byte[] packet(boolean connected){ByteBuffer b=ByteBuffer.allocate(32).order(ByteOrder.BIG_ENDIAN);b.putInt(0x4d454931).putInt(++sequence).putInt(connected?1:0).putInt(connected?keys|touch|hat:0);for(short a:axes)b.putShort(connected?a:0);b.put((byte)(connected?lt:0)).put((byte)(connected?rt:0));return b.array();}
 private synchronized byte[] playerPacket(){ByteBuffer b=ByteBuffer.allocate(32).order(ByteOrder.BIG_ENDIAN);b.putInt(0x4d455031).putInt(0).putLong(localPlayer).put(new byte[]{82,117,110,110,101,114});return b.array();}
 private void sendLoop(){Thread owner=Thread.currentThread();try(DatagramSocket socket=new DatagramSocket()){
  InetAddress address=InetAddress.getByName("127.0.0.1");try{while(active&&sender==owner){byte[] identity=playerPacket();socket.send(new DatagramPacket(identity,identity.length,address,38762));byte[] p=packet(true);socket.send(new DatagramPacket(p,p.length,address,38762));Thread.sleep(40);}}
  catch(InterruptedException ignored){}finally{byte[] p=packet(false);socket.send(new DatagramPacket(p,p.length,address,38762));}
 }catch(java.io.IOException ignored){/* Native timeout releases input on transport loss. */}}
 static int keyBit(int code){switch(code){
 case KeyEvent.KEYCODE_BUTTON_START:case KeyEvent.KEYCODE_ENTER:return 0x10;
 case KeyEvent.KEYCODE_BUTTON_SELECT:return 0x20;
 case KeyEvent.KEYCODE_BUTTON_A:case KeyEvent.KEYCODE_SPACE:return 0x1000;
 case KeyEvent.KEYCODE_BUTTON_B:case KeyEvent.KEYCODE_ESCAPE:return 0x2000;
 case KeyEvent.KEYCODE_BUTTON_X:return 0x4000;case KeyEvent.KEYCODE_BUTTON_Y:return 0x8000;
 case KeyEvent.KEYCODE_BUTTON_L1:return 0x100;case KeyEvent.KEYCODE_BUTTON_R1:return 0x200;
 case KeyEvent.KEYCODE_BUTTON_THUMBL:return 0x40;case KeyEvent.KEYCODE_BUTTON_THUMBR:return 0x80;
 case KeyEvent.KEYCODE_DPAD_UP:return 1;case KeyEvent.KEYCODE_DPAD_DOWN:return 2;case KeyEvent.KEYCODE_DPAD_LEFT:return 4;case KeyEvent.KEYCODE_DPAD_RIGHT:return 8;
 default:return 0;}}
 private static short axis(float f){if(!Float.isFinite(f))return 0;float a=Math.abs(f);return (short)(a<.18f?0:Math.round(Math.copySign(Math.min(1,(a-.18f)/.82f),f)*32767));}
 private static int trigger(MotionEvent e,int axis){InputDevice d=e.getDevice();InputDevice.MotionRange r=d==null?null:d.getMotionRange(axis,e.getSource());if(r==null)return 0;return Math.round(InputState.normalizeTrigger(e.getAxisValue(axis),r.getMin(),r.getMax())*255);}
 synchronized boolean motion(MotionEvent e){if((e.getSource()&InputDevice.SOURCE_JOYSTICK)!=InputDevice.SOURCE_JOYSTICK || e.getAction()!=MotionEvent.ACTION_MOVE)return false;
 axes[0]=axis(e.getAxisValue(MotionEvent.AXIS_X));axes[1]=axis(-e.getAxisValue(MotionEvent.AXIS_Y));axes[2]=axis(e.getAxisValue(MotionEvent.AXIS_Z));axes[3]=axis(-e.getAxisValue(MotionEvent.AXIS_RZ));
 float x=e.getAxisValue(MotionEvent.AXIS_HAT_X),y=e.getAxisValue(MotionEvent.AXIS_HAT_Y);hat=(x<-.5f?4:x>.5f?8:0)|(y<-.5f?1:y>.5f?2:0);
 lt=Math.max(trigger(e,MotionEvent.AXIS_LTRIGGER),trigger(e,MotionEvent.AXIS_BRAKE));rt=Math.max(trigger(e,MotionEvent.AXIS_RTRIGGER),trigger(e,MotionEvent.AXIS_GAS));return true;}
}
