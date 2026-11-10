import org.mirrorsedge.research.InputState;
public class InputStateTest {
 public static void main(String[] args) {
  InputState s=new InputState();
  check(s.touchVisible(),"touch default");
  s.moveX=1;s.buttons=InputState.JUMP;s.connect(true);
  check(!s.touchVisible()&&s.moveX==0&&s.buttons==0,"connect clears old input");
  s.moveX=1;s.lookY=1;s.buttons=15;s.connect(false);
  check(s.touchVisible()&&s.paused&&s.buttons==0&&s.lookY==0,"disconnect pauses and clears");
  s.resume();check(!s.paused&&s.moveX==0,"resume starts neutral");
  s.connect(true);s.forceTouch=true;check(s.touchVisible(),"manual touch override");
  check(s.axis(.1f)==0&&s.axis(-.1f)==0,"deadzone");
  check(s.axis(1)==1&&s.axis(-1)==-1&&s.axis(2)==1,"axis bounds");
  check(Math.abs(s.axis(.5f)+s.axis(-.5f))<.00001,"axis symmetry");
  s.moveX=1;s.buttons=1;s.pause();check(s.paused&&s.buttons==0&&s.moveX==0,"focus pause clears");
  s=new InputState();s.devices(false,true,false);check(!s.touchVisible(),"keyboard alone hides touch");
  s.devices(false,true,true);check(!s.touchVisible(),"keyboard and mouse hide touch");
  s.buttons=1;s.devices(false,false,true);check(!s.touchVisible()&&s.paused&&s.buttons==0,"remaining mouse keeps touch hidden, disconnect clears");
  s.resume();s.devices(false,false,false);check(s.touchVisible()&&s.paused,"last peripheral disconnect restores touch and pauses");
  s.devices(false,false,true);s.forceTouch=true;check(s.touchVisible(),"mouse manual override");
  for(int devices=0;devices<8;devices++){s=new InputState();s.devices((devices&1)!=0,(devices&2)!=0,(devices&4)!=0);check(s.touchVisible()==(devices==0),"all peripheral combinations");}
  check(InputState.hatButtons(0,0)==0,"neutral HAT");
  check(InputState.hatButtons(-1,0)==InputState.LEFT,"left HAT");
  check(InputState.hatButtons(1,0)==InputState.RIGHT,"right HAT");
  check(InputState.hatButtons(0,-1)==InputState.UP,"up HAT");
  check(InputState.hatButtons(0,1)==InputState.DOWN,"down HAT");
  check(InputState.hatButtons(-1,-1)==(InputState.LEFT|InputState.UP),"diagonal HAT");
  check(InputState.hatButtons(.1f,-.1f)==0,"HAT neutral tolerance");
  s=new InputState();s.buttons=InputState.A|InputState.hatButtons(1,1);check(s.buttonNames().contains("A")&&s.buttonNames().contains("↓")&&s.buttonNames().contains("→"),"readable simultaneous controls");
  s.buttons=InputState.A|InputState.hatButtons(0,0);check(s.buttons==InputState.A,"HAT release preserves digital button");
  check(InputState.normalizeTrigger(-1,-1,1)==0&&InputState.normalizeTrigger(0,-1,1)==.5f&&InputState.normalizeTrigger(1,-1,1)==1,"signed trigger range");
  check(InputState.normalizeTrigger(128,0,255)>0.5f&&InputState.normalizeTrigger(2,0,1)==1,"range and clamp");
  check(InputState.normalizeTrigger(Float.NaN,0,1)==0&&InputState.normalizeTrigger(1,1,1)==0,"invalid trigger input");
  s=new InputState();s.buttonsWithTriggers(InputState.A,.5f,.75f);check(s.leftTrigger==.5f&&s.rightTrigger==.75f&&s.buttons==(InputState.A|InputState.LT|InputState.RT),"analog triggers preserve pressure and buttons");
  s.buttonsWithTriggers(InputState.LT,0,0);check(s.leftTrigger==1&&s.buttons==InputState.LT,"digital trigger fallback");
  s.buttonsWithTriggers(InputState.A,0,0);check(s.leftTrigger==0&&s.rightTrigger==0&&s.buttons==InputState.A,"trigger release preserves A");
  s.pause();check(s.leftTrigger==0&&s.rightTrigger==0,"pause clears triggers");
  System.out.println("PASS: input transitions, HAT directions/diagonal/neutral, simultaneous buttons");
 }
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
}
