package org.mirrorsedge.research;

import android.app.*;
import android.os.*;
import android.content.*;
import android.content.res.Configuration;
import android.graphics.*;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;

public class MainActivity extends Activity implements InputManager.InputDeviceListener {
    private static native int nativeProbe(int a, int b);
    private final InputState input=new InputState();
    private final ExecutorService io=Executors.newSingleThreadExecutor();
    private final Set<Integer> padIds=new HashSet<>();
    private final Set<Integer> keyboardIds=new HashSet<>(),mouseIds=new HashSet<>(),heldKeys=new HashSet<>();
    private int padButtons=0, padHatButtons=0, mouseButtons=0;
    private float padLeftTrigger=0,padRightTrigger=0;
    private boolean analogLeftAvailable=false,analogRightAvailable=false;
    private int mouseGeneration=0;
    private TextView status,saveStatus;
    private InputView view;
    private InputManager manager;
    private boolean alive=true;
    private static final int OPEN=10, EXPORT=11;
    private String padName="нет";
    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root=new LinearLayout(this); root.setOrientation(1);root.setPadding(20,8,20,8);root.setBackgroundColor(0xff111820);
        root.setOnApplyWindowInsetsListener((v,insets)->{v.setPadding(20+insets.getSystemWindowInsetLeft(),8+insets.getSystemWindowInsetTop(),20+insets.getSystemWindowInsetRight(),8+insets.getSystemWindowInsetBottom());return insets;});
        status=text(root,"Проверка JNI…");
        try { System.loadLibrary("meprobe"); int result=nativeProbe(7,3);status.setText("ARM64/JNI: "+(result==76?"PASS":"FAIL")+" · Android "+Build.VERSION.RELEASE+" · "+Build.MODEL+" · "+Arrays.toString(Build.SUPPORTED_ABIS)); }
        catch(Throwable e) {status.setText("JNI FAIL: "+e.getClass().getSimpleName()+" "+e.getMessage());}
        text(root,"Технический стенд ввода и файлов. Игровой runtime запускается отдельно.");
        Button preview=new Button(this);preview.setText("Живой просмотр игры (диагностика)");root.addView(preview);
        preview.setOnClickListener(v->startActivity(new Intent(this,PreviewActivity.class)));
        Button play=new Button(this);play.setText("Играть (вывод на экран)");root.addView(play);
        play.setOnClickListener(v->startActivity(new Intent(this,GameActivity.class)));
        LinearLayout options=new LinearLayout(this);root.addView(options);
        CheckBox intro=new CheckBox(this);intro.setTextColor(Color.WHITE);intro.setText("Пропускать стартовую заставку (будущий запуск)");
        intro.setChecked(getPreferences(0).getBoolean("skipIntro",false));options.addView(intro);
        intro.setOnCheckedChangeListener((b1,on)->getPreferences(0).edit().putBoolean("skipIntro",on).apply());
        CheckBox touch=new CheckBox(this);touch.setTextColor(Color.WHITE);touch.setText("Экранное управление вручную");options.addView(touch);
        touch.setOnCheckedChangeListener((b1,on)->{input.clear();input.forceTouch=on;view.resetPointers();if(on)view.releasePointerCapture();view.invalidate();});
        Button resume=new Button(this);resume.setText("Продолжить тест");options.addView(resume);resume.setOnClickListener(v->{view.resetPointers();input.resume();view.requestFocus();view.invalidate();});
        LinearLayout files=new LinearLayout(this);root.addView(files);
        Button open=new Button(this);open.setText("Выбрать PC-save");files.addView(open);
        open.setOnClickListener(v->new AlertDialog.Builder(this).setMessage("Будет сохранена копия файла для теста хранения. Формат и загрузка игрового прогресса ещё не проверяются. Исходный файл не изменяется.")
            .setPositiveButton("Выбрать",(d,w)->{input.pause();view.resetPointers();Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.setType("*/*");i.addCategory(Intent.CATEGORY_OPENABLE);startActivityForResult(i,OPEN);}).setNegativeButton("Отмена",null).show());
        Button export=new Button(this);export.setText("Экспорт копии");files.addView(export);
        export.setOnClickListener(v->{if(!saveFile().exists()){saveStatus.setText("Сначала выбери файл.");return;}Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT);i.setType("application/octet-stream");i.addCategory(Intent.CATEGORY_OPENABLE);i.putExtra(Intent.EXTRA_TITLE,"mirrors-edge-pc-save.dat");startActivityForResult(i,EXPORT);});
        saveStatus=text(files,"Совместимость PC-save ещё не подтверждена.");
        view=new InputView();root.addView(view,new LinearLayout.LayoutParams(-1,0,1));setContentView(root);
        manager=(InputManager)getSystemService(INPUT_SERVICE);manager.registerInputDeviceListener(this,null);refreshPads();
    }
    private TextView text(LinearLayout parent,String label){TextView t=new TextView(this);t.setTextColor(Color.WHITE);t.setText(label);t.setTextSize(12);parent.addView(t);return t;}
    private File saveFile(){return new File(getFilesDir(),"imported-pc-save.dat");}
    private void refreshPads(){
        padIds.clear();keyboardIds.clear();mouseIds.clear();padName="нет";
        for(int id:InputDevice.getDeviceIds()){InputDevice d=InputDevice.getDevice(id);if(d==null||d.isVirtual())continue;
            if(d.supportsSource(InputDevice.SOURCE_GAMEPAD)||d.supportsSource(InputDevice.SOURCE_JOYSTICK)){padIds.add(id);padName=d.getName();}
            if(d.getKeyboardType()==InputDevice.KEYBOARD_TYPE_ALPHABETIC&&d.supportsSource(InputDevice.SOURCE_KEYBOARD))keyboardIds.add(id);
            if(d.supportsSource(InputDevice.SOURCE_MOUSE)||d.supportsSource(InputDevice.SOURCE_MOUSE_RELATIVE))mouseIds.add(id);
        }
        input.devices(!padIds.isEmpty(),!keyboardIds.isEmpty(),!mouseIds.isEmpty());if(view!=null){view.resetPointers();if(!input.mouse||input.paused)view.releasePointerCapture();view.invalidate();}
    }
    public void onInputDeviceAdded(int id){refreshPads();}
    public void onInputDeviceRemoved(int id){if(padIds.contains(id)||keyboardIds.contains(id)||mouseIds.contains(id)){input.pause();input.clear();}refreshPads();}
    public void onInputDeviceChanged(int id){input.clear();refreshPads();}
    private boolean padEvent(int id){return padIds.contains(id)&&!input.forceTouch;}
    private void togglePause(){boolean was=input.paused;view.resetPointers();if(was){input.resume();view.requestFocus();}else{input.pause();view.releasePointerCapture();}view.invalidate();}
    private int keyboardAction(int key){switch(key){case KeyEvent.KEYCODE_SPACE:return InputState.A;case KeyEvent.KEYCODE_CTRL_LEFT:case KeyEvent.KEYCODE_CTRL_RIGHT:return InputState.B;case KeyEvent.KEYCODE_E:return InputState.X;case KeyEvent.KEYCODE_R:return InputState.Y;case KeyEvent.KEYCODE_Q:return InputState.LB;case KeyEvent.KEYCODE_SHIFT_LEFT:case KeyEvent.KEYCODE_SHIFT_RIGHT:return InputState.RB;case KeyEvent.KEYCODE_TAB:return InputState.BACK;default:return 0;}}
    private void updateKeyboard(){
        input.moveX=(heldKeys.contains(KeyEvent.KEYCODE_D)?1:0)-(heldKeys.contains(KeyEvent.KEYCODE_A)?1:0);
        input.moveY=(heldKeys.contains(KeyEvent.KEYCODE_S)?1:0)-(heldKeys.contains(KeyEvent.KEYCODE_W)?1:0);
        updateButtons();
    }
    private void updateButtons(){int digital=padButtons;if(analogLeftAvailable)digital&=~InputState.LT;if(analogRightAvailable)digital&=~InputState.RT;int bits=digital|padHatButtons|mouseButtons;for(int key:heldKeys)bits|=keyboardAction(key);input.buttonsWithTriggers(bits,padLeftTrigger,padRightTrigger);}
    private boolean hasAxis(MotionEvent e,int axis){return e.getDevice()!=null&&e.getDevice().getMotionRange(axis)!=null;}
    private float triggerAxis(MotionEvent e,int axis){InputDevice d=e.getDevice();if(d==null)return 0;InputDevice.MotionRange r=d.getMotionRange(axis,e.getSource());if(r==null)r=d.getMotionRange(axis);return r==null?0:InputState.normalizeTrigger(e.getAxisValue(axis),r.getMin(),r.getMax());}
    @Override public boolean onGenericMotionEvent(MotionEvent e){
        if(padEvent(e.getDeviceId())&&(e.getSource()&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK){
            if(!input.paused){input.moveX=input.axis(e.getAxisValue(MotionEvent.AXIS_X));input.moveY=input.axis(e.getAxisValue(MotionEvent.AXIS_Y));input.lookX=input.axis(e.getAxisValue(MotionEvent.AXIS_Z));input.lookY=input.axis(e.getAxisValue(MotionEvent.AXIS_RZ));analogLeftAvailable=hasAxis(e,MotionEvent.AXIS_LTRIGGER)||hasAxis(e,MotionEvent.AXIS_BRAKE);analogRightAvailable=hasAxis(e,MotionEvent.AXIS_RTRIGGER)||hasAxis(e,MotionEvent.AXIS_GAS);padLeftTrigger=Math.max(triggerAxis(e,MotionEvent.AXIS_LTRIGGER),triggerAxis(e,MotionEvent.AXIS_BRAKE));padRightTrigger=Math.max(triggerAxis(e,MotionEvent.AXIS_RTRIGGER),triggerAxis(e,MotionEvent.AXIS_GAS));padHatButtons=InputState.hatButtons(e.getAxisValue(MotionEvent.AXIS_HAT_X),e.getAxisValue(MotionEvent.AXIS_HAT_Y));updateButtons();}view.invalidate();return true;}
        return super.onGenericMotionEvent(e);
    }
    private int action(int key){switch(key){case KeyEvent.KEYCODE_BUTTON_A:return InputState.A;case KeyEvent.KEYCODE_BUTTON_B:return InputState.B;case KeyEvent.KEYCODE_BUTTON_X:return InputState.X;case KeyEvent.KEYCODE_BUTTON_Y:return InputState.Y;case KeyEvent.KEYCODE_BUTTON_L1:return InputState.LB;case KeyEvent.KEYCODE_BUTTON_R1:return InputState.RB;case KeyEvent.KEYCODE_BUTTON_L2:return InputState.LT;case KeyEvent.KEYCODE_BUTTON_R2:return InputState.RT;case KeyEvent.KEYCODE_BUTTON_SELECT:return InputState.BACK;case KeyEvent.KEYCODE_DPAD_UP:return InputState.UP;case KeyEvent.KEYCODE_DPAD_DOWN:return InputState.DOWN;case KeyEvent.KEYCODE_DPAD_LEFT:return InputState.LEFT;case KeyEvent.KEYCODE_DPAD_RIGHT:return InputState.RIGHT;default:return 0;}}
    @Override public boolean dispatchKeyEvent(KeyEvent e){
        if(keyboardIds.contains(e.getDeviceId())&&!input.forceTouch){int key=e.getKeyCode();
            if(key==KeyEvent.KEYCODE_ESCAPE){if(e.getAction()==KeyEvent.ACTION_DOWN&&e.getRepeatCount()==0)togglePause();return true;}
            if(key==KeyEvent.KEYCODE_W||key==KeyEvent.KEYCODE_A||key==KeyEvent.KEYCODE_S||key==KeyEvent.KEYCODE_D||keyboardAction(key)!=0){
                if(e.getAction()==KeyEvent.ACTION_UP)heldKeys.remove(key);else if(e.getAction()==KeyEvent.ACTION_DOWN&&!input.paused)heldKeys.add(key);updateKeyboard();view.invalidate();return true;}}
        if(padEvent(e.getDeviceId())){
            if(e.getKeyCode()==KeyEvent.KEYCODE_BUTTON_START&&e.getAction()==KeyEvent.ACTION_DOWN&&e.getRepeatCount()==0){togglePause();return true;}
            int bit=action(e.getKeyCode());if(bit!=0){if(e.getAction()==KeyEvent.ACTION_UP)padButtons&=~bit;else if(e.getAction()==KeyEvent.ACTION_DOWN&&!input.paused)padButtons|=bit;updateButtons();view.invalidate();return true;}}
        return super.dispatchKeyEvent(e);
    }
    @Override public void onWindowFocusChanged(boolean focus){super.onWindowFocusChanged(focus);if(!focus){input.pause();if(view!=null){view.resetPointers();view.releasePointerCapture();}}else if(manager!=null)refreshPads();}
    @Override protected void onPause(){super.onPause();input.pause();if(view!=null)view.resetPointers();}
    @Override protected void onDestroy(){alive=false;if(manager!=null)manager.unregisterInputDeviceListener(this);io.shutdown();super.onDestroy();}
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);if(result!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();saveStatus.setText("Копирование…");io.execute(()->{String message;try{if(request==OPEN){
            File temp=new File(getFilesDir(),"save-import.tmp");long count=0;MessageDigest digest=MessageDigest.getInstance("SHA-256");
            try(InputStream in=getContentResolver().openInputStream(uri);OutputStream out=new FileOutputStream(temp)){if(in==null)throw new IOException("Нет потока");byte[] bytes=new byte[65536];int n;while((n=in.read(bytes))!=-1){count+=n;if(count>16L*1024*1024)throw new IOException("Лимит теста: 16 МиБ");digest.update(bytes,0,n);out.write(bytes,0,n);}}
            if(count==0)throw new IOException("Пустой файл");
            if(saveFile().exists())Files.copy(saveFile().toPath(),new File(getFilesDir(),"previous-pc-save.dat").toPath(),StandardCopyOption.REPLACE_EXISTING);
            Files.move(temp.toPath(),saveFile().toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
            StringBuilder hash=new StringBuilder();for(byte v:digest.digest())hash.append(String.format(Locale.ROOT,"%02x",v&255));message="Копия: "+count+" байт · SHA256 "+hash.substring(0,12)+"… · загрузка игры не проверена";
        }else if(request==EXPORT){try(InputStream in=new FileInputStream(saveFile());OutputStream out=getContentResolver().openOutputStream(uri,"w")){if(out==null)throw new IOException("Нет потока");byte[] bytes=new byte[65536];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);}message="Копия экспортирована без изменения формата.";}else return;
        }catch(Exception e){new File(getFilesDir(),"save-import.tmp").delete();message="Ошибка: "+e.getMessage();}final String msg=message;runOnUiThread(()->{if(alive)saveStatus.setText(msg);});});
    }
    private class InputView extends View {
        private final Paint paint=new Paint(3);private final Map<Integer,Integer> roles=new HashMap<>();
        InputView(){super(MainActivity.this);setFocusable(true);setFocusableInTouchMode(true);}
        void resetPointers(){roles.clear();heldKeys.clear();padButtons=padHatButtons=mouseButtons=0;padLeftTrigger=padRightTrigger=0;analogLeftAvailable=analogRightAvailable=false;mouseGeneration++;input.clear();invalidate();}
        @Override public boolean onCapturedPointerEvent(MotionEvent e){
            if(input.paused||input.forceTouch||!input.mouse){releasePointerCapture();return true;}
            if((e.getButtonState()&MotionEvent.BUTTON_TERTIARY)!=0){input.pause();resetPointers();releasePointerCapture();return true;}
            if(e.getActionMasked()==MotionEvent.ACTION_MOVE){input.lookX=e.getX()*.015f;input.lookY=e.getY()*.015f;final int generation=++mouseGeneration;postDelayed(()->{if(generation==mouseGeneration){input.lookX=input.lookY=0;invalidate();}},100);}
            mouseButtons=((e.getButtonState()&MotionEvent.BUTTON_PRIMARY)!=0?InputState.RT:0)|((e.getButtonState()&MotionEvent.BUTTON_SECONDARY)!=0?InputState.LT:0);updateButtons();invalidate();return true;
        }
        @Override public void onPointerCaptureChange(boolean captured){super.onPointerCaptureChange(captured);if(!captured){mouseGeneration++;mouseButtons=0;input.lookX=input.lookY=0;updateButtons();invalidate();}}
        private final String[] labels={"L","R","A","B","X","Y","LB","RB","LT","RT","BACK","START","↑","↓","←","→"};
        private final int[] bits={0,0,InputState.A,InputState.B,InputState.X,InputState.Y,InputState.LB,InputState.RB,InputState.LT,InputState.RT,InputState.BACK,0,InputState.UP,InputState.DOWN,InputState.LEFT,InputState.RIGHT};
        private float unit(){return Math.min(getWidth()/18f,getHeight()/7f);}
        private float radius(){return unit()*1.15f;}
        private float cx(int role){float w=getWidth(),u=unit();switch(role){case 0:return w*.13f;case 1:return w*.70f;case 2:case 5:return w*.85f;case 3:return w*.85f+u*1.3f;case 4:return w*.85f-u*1.3f;case 6:return w*.10f;case 7:return w*.90f;case 8:return w*.23f;case 9:return w*.77f;case 10:return w*.44f;case 11:return w*.56f;case 12:case 13:return w*.30f;case 14:return w*.30f-u;default:return w*.30f+u;}}
        private float cy(int role){float h=getHeight(),u=unit();switch(role){case 0:return h*.54f;case 1:return h*.77f;case 2:return h*.48f+u*1.3f;case 5:return h*.48f-u*1.3f;case 3:case 4:return h*.48f;case 6:case 7:case 8:case 9:return h*.21f;case 10:case 11:return h*.38f;case 12:return h*.77f-u;case 13:return h*.77f+u;default:return h*.77f;}}
        private float hitRadius(int role){return role<2?radius():unit()*(role>=12?.43f:.57f);}
        private void control(Canvas c,int role){float x=cx(role),y=cy(role),r=hitRadius(role);boolean pressed=roles.containsValue(role);int color=role==2?0xff62c45c:role==3?0xffe15d56:role==4?0xff58a8ec:role==5?0xffe3c954:0xffd5e1e7;
            paint.setStyle(Paint.Style.FILL);paint.setColor(pressed?0x665aa1c4:0x262c3c48);c.drawCircle(x,y,r,paint);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(unit()*.035f);paint.setColor(color);c.drawCircle(x,y,r,paint);paint.setStyle(Paint.Style.FILL);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(unit()*(role==10||role==11?.25f:.43f));c.drawText(labels[role],x,y-(paint.ascent()+paint.descent())/2,paint);
            if(role<2){float dx=role==0?input.moveX:input.lookX,dy=role==0?input.moveY:input.lookY;paint.setColor(0x9985b6cc);c.drawCircle(x+dx*r*.55f,y+dy*r*.55f,r*.33f,paint);}}
        @Override protected void onDraw(Canvas c){super.onDraw(c);paint.setColor(Color.WHITE);paint.setTextSize(26);c.drawText("Геймпад: "+padName+" · клавиатура: "+(input.keyboard?"да":"нет")+" · мышь: "+(input.mouse?"да":"нет")+" · "+(input.paused?"ПАУЗА":"ТЕСТ ВВОДА"),16,32,paint);
            c.drawText(String.format(Locale.ROOT,"Ход %.2f %.2f · камера %.2f %.2f · LT %.2f RT %.2f · кнопки: %s",input.moveX,input.moveY,input.lookX,input.lookY,input.leftTrigger,input.rightTrigger,input.buttonNames()),16,66,paint);
            if(input.touchVisible()){for(int role=0;role<labels.length;role++)control(c,role);}else{paint.setTextSize(30);c.drawText(input.mouse?"Клик здесь — захват мыши · Esc / средняя кнопка — освобождение":"Внешнее управление подключено — экранные кнопки скрыты",16,110,paint);}paint.setTextAlign(Paint.Align.LEFT);}
        @Override public boolean onTouchEvent(MotionEvent e){
            if(e.isFromSource(InputDevice.SOURCE_MOUSE)&&input.mouse&&!input.forceTouch){if(e.getActionMasked()==MotionEvent.ACTION_DOWN&&!input.paused){requestFocus();requestPointerCapture();}return true;}
            if(!input.touchVisible())return true;
            int action=e.getActionMasked(),index=e.getActionIndex();float r=radius();
            if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN){float x=e.getX(index),y=e.getY(index);if(Math.hypot(x-cx(11),y-cy(11))<hitRadius(11)){togglePause();return true;}
                if(!input.paused)for(int role=labels.length-1;role>=0;role--){if(role!=11&&Math.hypot(x-cx(role),y-cy(role))<hitRadius(role)&&!roles.containsValue(role)){roles.put(e.getPointerId(index),role);break;}}}
            if(input.paused)return true;
            if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)roles.remove(e.getPointerId(index));
            if(action==MotionEvent.ACTION_CANCEL){resetPointers();return true;}
            input.clear();for(int i=0;i<e.getPointerCount();i++){Integer role=roles.get(e.getPointerId(i));if(role==null)continue;
                if(role==0){input.moveX=input.axis((e.getX(i)-cx(0))/r);input.moveY=input.axis((e.getY(i)-cy(0))/r);}else if(role==1){input.lookX=input.axis((e.getX(i)-cx(1))/r);input.lookY=input.axis((e.getY(i)-cy(1))/r);}else input.buttons|=bits[role];}input.buttonsWithTriggers(input.buttons,0,0);
            invalidate();return true;
        }
    }
}




