package org.mirrorsedge.research;
import android.view.*;
import java.net.*;
import java.nio.*;
/** Loopback input for user zero. No profile or save service is implied. */
final class GuestInput {
 private volatile boolean active;private Thread sender;private int keys,touch,hat,sequence;private final short[] axes=new short[4];private int lt,rt;private long localPlayer;
 // On-screen controls (TouchPad): both sticks and the triggers.
 // A physical pad wins where it is non-neutral.
 private final short[] touchAxes=new short[4];private int touchLt,touchRt;
 // Taps shorter than the 40 ms packet interval: a press is kept until one packet carried it.
 private int touchLatch;private boolean ltLatch,rtLatch;
 synchronized void player(long xuid){localPlayer=xuid;}
 synchronized void button(int bit,boolean down,boolean virtual){if(virtual){if(down){touch|=bit;touchLatch|=bit;}else touch&=~bit;}else keys=down?keys|bit:keys&~bit;}
 /** Left stick from the screen, x right and y down in -1..1. */
 synchronized void touchStick(float x,float y){touchAxes[0]=full(x);touchAxes[1]=full(-y);}
 /** Right stick from the screen (camera), x right and y down in -1..1. */
 synchronized void touchRightStick(float x,float y){touchAxes[2]=full(x);touchAxes[3]=full(-y);}
 synchronized void touchTrigger(boolean left,boolean down){if(left){touchLt=down?255:0;ltLatch|=down;}else{touchRt=down?255:0;rtLatch|=down;}}
 synchronized void clear(){keys=touch=hat=lt=rt=keyLt=keyRt=touchLt=touchRt=touchLatch=0;ltLatch=rtLatch=false;java.util.Arrays.fill(axes,(short)0);java.util.Arrays.fill(touchAxes,(short)0);}
 void start(){active=true;Thread owner=new Thread(this::sendLoop,"guest-input");sender=owner;owner.start();}
 void stop(){active=false;clear();Thread old=sender;if(old!=null)old.interrupt();}
 private static short full(float f){if(!Float.isFinite(f))return 0;return (short)Math.round(Math.max(-1,Math.min(1,f))*32767);}
 private synchronized byte[] packet(boolean connected){
  short[] merged=new short[4];for(int i=0;i<4;++i)merged[i]=axes[i]!=0?axes[i]:touchAxes[i];
  ByteBuffer b=ByteBuffer.allocate(32).order(ByteOrder.BIG_ENDIAN);b.putInt(0x4d454931).putInt(++sequence).putInt(connected?1:0).putInt(connected?keys|touch|touchLatch|hat:0);for(short a:merged)b.putShort(connected?a:0);b.put((byte)(connected?Math.max(Math.max(lt,keyLt),ltLatch?255:touchLt):0)).put((byte)(connected?Math.max(Math.max(rt,keyRt),rtLatch?255:touchRt):0));
  if(connected){touchLatch=0;ltLatch=rtLatch=false;}return b.array();}
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
 /** Digital triggers (KEYCODE_BUTTON_L2/R2: Switch ZL/ZR and many generic pads) as a full pull. */
 private int keyLt,keyRt;
 synchronized void triggerKey(boolean left,boolean down){if(left)keyLt=down?255:0;else keyRt=down?255:0;}
 static boolean isTriggerKey(int code){return code==KeyEvent.KEYCODE_BUTTON_L2||code==KeyEvent.KEYCODE_BUTTON_R2;}
 synchronized boolean motion(MotionEvent e){return motion(e,MotionEvent.AXIS_Z,MotionEvent.AXIS_RZ);}
 /** Gamepad axes; the right stick axes come from the pad's layout (Controller). */
 synchronized boolean motion(MotionEvent e,int rightX,int rightY){if((e.getSource()&InputDevice.SOURCE_JOYSTICK)!=InputDevice.SOURCE_JOYSTICK || e.getAction()!=MotionEvent.ACTION_MOVE)return false;
 axes[0]=axis(e.getAxisValue(MotionEvent.AXIS_X));axes[1]=axis(-e.getAxisValue(MotionEvent.AXIS_Y));axes[2]=axis(e.getAxisValue(rightX));axes[3]=axis(-e.getAxisValue(rightY));
 float x=e.getAxisValue(MotionEvent.AXIS_HAT_X),y=e.getAxisValue(MotionEvent.AXIS_HAT_Y);hat=(x<-.5f?4:x>.5f?8:0)|(y<-.5f?1:y>.5f?2:0);
 lt=Math.max(trigger(e,MotionEvent.AXIS_LTRIGGER),trigger(e,MotionEvent.AXIS_BRAKE));rt=Math.max(trigger(e,MotionEvent.AXIS_RTRIGGER),trigger(e,MotionEvent.AXIS_GAS));return true;}
}
