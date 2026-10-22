package org.mirrorsedge.research;

public final class InputState {
    public static final int JUMP=1, CROUCH=2, ACTION=4, ATTACK=8;
    public static final int A=1, B=2, X=4, Y=8, LB=16, RB=32, LT=64, RT=128,
        BACK=256, UP=512, DOWN=1024, LEFT=2048, RIGHT=4096;
    public float moveX, moveY, lookX, lookY;
    public float leftTrigger,rightTrigger;
    public int buttons;
    public boolean controller, keyboard, mouse, forceTouch, paused;
    public float deadzone = 0.18f;
    public boolean touchVisible() { return !(controller || keyboard || mouse) || forceTouch; }
    public static int hatButtons(float x, float y) {
        int bits=0;
        if(x < -.5f) bits|=LEFT; else if(x > .5f) bits|=RIGHT;
        if(y < -.5f) bits|=UP; else if(y > .5f) bits|=DOWN;
        return bits;
    }
    public String buttonNames() {
        int[] bits={A,B,X,Y,LB,RB,LT,RT,BACK,UP,DOWN,LEFT,RIGHT};
        String[] names={"A","B","X","Y","LB","RB","LT","RT","BACK","↑","↓","←","→"};
        StringBuilder text=new StringBuilder();
        for(int i=0;i<bits.length;i++)if((buttons&bits[i])!=0){if(text.length()>0)text.append(' ');text.append(names[i]);}
        return text.length()==0?"—":text.toString();
    }
    public float axis(float value) {
        float a=Math.min(1f, Math.abs(value));
        return a<=deadzone ? 0 : Math.copySign((a-deadzone)/(1-deadzone), value);
    }
    public static float normalizeTrigger(float value,float min,float max) {
        if(!Float.isFinite(value)||!Float.isFinite(min)||!Float.isFinite(max)||max<=min)return 0;
        return Math.max(0,Math.min(1,(value-min)/(max-min)));
    }
    public void buttonsWithTriggers(int digital,float left,float right) {
        if(paused){buttons=0;leftTrigger=rightTrigger=0;return;}
        leftTrigger=Math.max(normalizeTrigger(left,0,1),(digital&LT)!=0?1:0);
        rightTrigger=Math.max(normalizeTrigger(right,0,1),(digital&RT)!=0?1:0);
        buttons=digital|(leftTrigger>.1f?LT:0)|(rightTrigger>.1f?RT:0);
    }
    public void clear() { moveX=moveY=lookX=lookY=leftTrigger=rightTrigger=0; buttons=0; }
    public void connect(boolean connected) {
        devices(connected,keyboard,mouse);
    }
    public void devices(boolean pad, boolean keys, boolean pointer) {
        if(controller!=pad || keyboard!=keys || mouse!=pointer) {
            clear();
            if((controller&&!pad)||(keyboard&&!keys)||(mouse&&!pointer)) paused=true;
        }
        controller=pad;keyboard=keys;mouse=pointer;
    }
    public void pause() { clear(); paused=true; }
    public void resume() { clear(); paused=false; }
}
