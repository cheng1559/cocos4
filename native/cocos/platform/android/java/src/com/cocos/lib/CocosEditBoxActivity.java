/****************************************************************************
Copyright (c) 2010-2012 cocos2d-x.org
Copyright (c) 2013-2016 Chukong Technologies Inc.
Copyright (c) 2017-2018 Xiamen Yaji Software Co., Ltd.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
 ****************************************************************************/
package com.cocos.lib;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.StateListDrawable;
import android.graphics.drawable.shapes.RoundRectShape;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.Window;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

public class CocosEditBoxActivity extends Activity {

    // a color of dark green, was used for confirm button background
    private static final int DARK_GREEN = Color.parseColor("#1fa014");
    private static final int DARK_GREEN_PRESS = Color.parseColor("#008e26");
    private static final String ORIENTATION = "orientation";

    private static CocosEditBoxActivity sThis = null;
    private Cocos2dxEditText mEditText = null;
    private Button mButton = null;
    private String mButtonTitle = null;
    private boolean mConfirmHold = true;
    private int mEditTextID = 1;
    private int mButtonLayoutID = 2;
    private int mInitialSystemWindowInsetBottom = -1;

    /** The IME action for a confirm type; no fullscreen extract UI. */
    static int imeOptionsFor(String confirmType) {
        int action = confirmType.contentEquals("done") ? EditorInfo.IME_ACTION_DONE
            : confirmType.contentEquals("next") ? EditorInfo.IME_ACTION_NEXT
            : confirmType.contentEquals("search") ? EditorInfo.IME_ACTION_SEARCH
            : confirmType.contentEquals("go") ? EditorInfo.IME_ACTION_GO
            : confirmType.contentEquals("send") ? EditorInfo.IME_ACTION_SEND
            : EditorInfo.IME_ACTION_UNSPECIFIED;
        return action | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
    }

    /** Keyboard layout, suggestions and ASCII hints shared by both editors; call after setting the IME action. */
    static void applyInputTraits(EditText view, final String inputType, boolean isMultiLine, boolean autocorrect) {
        int type;
        if (inputType.contentEquals("text"))
            type = InputType.TYPE_CLASS_TEXT;
        // Most keyboards, including Chinese ones, switch to a plain English layout without
        // suggestions for visible passwords; FORCE_ASCII below covers the ones that honor it.
        else if (inputType.contentEquals("ascii"))
            type = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        else if (inputType.contentEquals("email"))
            type = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
        else if (inputType.contentEquals("url"))
            type = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI;
        else if (inputType.contentEquals("number"))
            type = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL | InputType.TYPE_NUMBER_FLAG_SIGNED;
        else if (inputType.contentEquals("phone"))
            type = InputType.TYPE_CLASS_PHONE;
        else if (inputType.contentEquals("password")) {
            type = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        } else {
            Log.e("Cocos2dxEditBox", "unknown input type " + inputType);
            type = InputType.TYPE_CLASS_TEXT;
        }
        if ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT) {
            if (isMultiLine) type |= InputType.TYPE_TEXT_FLAG_MULTI_LINE;
            if (!autocorrect) type |= InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        }
        view.setInputType(type);

        // Keyboards treat these as hints; setConfirmType has already set the action.
        int imeFlags = 0;
        if (inputType.contentEquals("ascii")) imeFlags |= EditorInfo.IME_FLAG_FORCE_ASCII;
        if (!autocorrect && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) imeFlags |= EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING;
        view.setImeOptions(view.getImeOptions() & ~(EditorInfo.IME_FLAG_FORCE_ASCII | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING) | imeFlags);
    }

    /***************************************************************************************
     Inner class.
     **************************************************************************************/
    class Cocos2dxEditText extends EditText {
        private final String TAG = "Cocos2dxEditBox";
        private boolean mIsMultiLine = false;
        private TextWatcher mTextWatcher = null;
        private boolean keyboardVisible = false;
        private int mScreenHeight;
        private boolean mCheckKeyboardShowNormally = false;

        public  Cocos2dxEditText(Activity context){
            super(context);
            this.setTextColor(Color.BLACK);
            mScreenHeight = ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).
                    getDefaultDisplay().getHeight();

            mTextWatcher = new TextWatcher() {
                @Override
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {

                }

                @Override
                public void onTextChanged(CharSequence s, int start, int before, int count) {

                }

                @Override
                public void afterTextChanged(Editable s) {
                    // Pass text to c++.
                    CocosEditBoxActivity.this.onKeyboardInput(s.toString());
                }
            };
            registKeyboardVisible();
        }

        /***************************************************************************************
         Public functions.
         **************************************************************************************/

        public void show(String defaultValue, int maxLength, boolean isMultiline, boolean confirmHold, String confirmType, String inputType, boolean autocorrect) {
            mIsMultiLine = isMultiline;
            this.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxLength) });
            this.setText(defaultValue);
            if (this.getText().length() >= defaultValue.length()) {
                this.setSelection(defaultValue.length());
            } else {
                this.setSelection(this.getText().length());
            }
            this.setConfirmType(confirmType);
            this.setInputType(inputType, mIsMultiLine, autocorrect);
            this.setVisibility(View.VISIBLE);

            // Open soft keyboard manually. Should request focus to open soft keyboard.
            this.requestFocus();

            this.addListeners();
        }

        public void hide() {
            mEditText.setVisibility(View.INVISIBLE);
            this.removeListeners();
        }

        /***************************************************************************************
         Private functions.
         **************************************************************************************/

        private void setConfirmType(final String confirmType) {
            int imeOptions = imeOptionsFor(confirmType);
            if (confirmType.contentEquals("done")) {
                this.setImeOptions(imeOptions);
                mButtonTitle = getResources().getString(R.string.done);
            } else if (confirmType.contentEquals("next")) {
                this.setImeOptions(imeOptions);
                mButtonTitle = getResources().getString(R.string.next);
            } else if (confirmType.contentEquals("search")) {
                this.setImeOptions(imeOptions);
                mButtonTitle = getResources().getString(R.string.search);
            } else if (confirmType.contentEquals("go")) {
                this.setImeOptions(imeOptions);
                mButtonTitle = getResources().getString(R.string.go);
            } else if (confirmType.contentEquals("send")) {
                this.setImeOptions(imeOptions);
                mButtonTitle = getResources().getString(R.string.send);
            } else{
                mButtonTitle = null;
                Log.e(TAG, "unknown confirm type " + confirmType);
            }
        }

        private void setInputType(final String inputType, boolean isMultiLine, boolean autocorrect){
            mCheckKeyboardShowNormally = inputType.contentEquals("password") && Build.BRAND.equalsIgnoreCase("oppo");
            applyInputTraits(this, inputType, isMultiLine, autocorrect);
        }

        private void addListeners() {

            this.setOnEditorActionListener(new TextView.OnEditorActionListener() {
                @Override
                public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                    if (! mIsMultiLine) {
                        CocosEditBoxActivity.this.hide();
                    }

                    return false; // pass on to other listeners.
                }
            });


            this.addTextChangedListener(mTextWatcher);
        }

        private void removeListeners() {
            this.setOnEditorActionListener(null);
            this.removeTextChangedListener(mTextWatcher);
        }

        private boolean isSystemAdjustUIWhenPopKeyboard() {
            int bottomOffset = 0;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                bottomOffset = getWindow().getDecorView().getRootWindowInsets().getSystemWindowInsetBottom();
            }
            // view will be scrolled to the target position by system,
            if (Math.abs(mInitialSystemWindowInsetBottom - bottomOffset) > 100) {
                return true;
            }
            return false;
        }

        private void registKeyboardVisible() {
            getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
                @Override
                public void onGlobalLayout() {
                    Rect r = new Rect();
                    getWindowVisibleDisplayFrame(r);
                    int heightDiff = getRootView().getHeight() - (r.bottom - r.top);
                    // if more than a quarter of the screen, its probably a keyboard
                    if (heightDiff > mScreenHeight/4) {
                        if (!keyboardVisible) {
                            keyboardVisible = true;
                        }
                        if (!isSystemAdjustUIWhenPopKeyboard()) {
                            getRootView().scrollTo(0, heightDiff);
                        }
                    } else {
                        getRootView().scrollTo(0, 0);
                        if (mCheckKeyboardShowNormally && !keyboardVisible) {
                            Toast.makeText(CocosEditBoxActivity.this, R.string.tip_disable_safe_input_type, Toast.LENGTH_SHORT).show();
                        }
                        if (keyboardVisible) {
                            keyboardVisible = false;
                            CocosEditBoxActivity.this.hide();
                        }
                    }
                }
            });
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        CocosEditBoxActivity.sThis = this;

        ViewGroup.LayoutParams frameLayoutParams =
                new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
        RelativeLayout frameLayout = new RelativeLayout(this);
        frameLayout.setLayoutParams(frameLayoutParams);
        frameLayout.setClickable(true);
        frameLayout.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                CocosEditBoxActivity.this.hide();
            }
        });
        setContentView(frameLayout);

        this.addItems(frameLayout);

        Intent intent = getIntent();
        Bundle extras = null;
        if (null != intent) {
            extras = intent.getExtras();
        }
        if(extras == null){
            show("",
                10,
                false,
                false,
                "done",
                "text",
                true
                );
        } else {
            int orientation = extras.getInt(ORIENTATION);
            setRequestedOrientation(orientation);
            show(extras.getString("defaultValue"),
                extras.getInt("maxLength"),
                extras.getBoolean("isMultiline"),
                extras.getBoolean("confirmHold"),
                extras.getString("confirmType"),
                extras.getString("inputType"),
                extras.getBoolean("autocorrect", true));
        }
    }

    @Override
    public void onAttachedToWindow() {
        View decorView = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            mInitialSystemWindowInsetBottom = decorView.getRootWindowInsets().getSystemWindowInsetBottom();
        }
    }

    /***************************************************************************************
     Public functions.
     **************************************************************************************/

    /***************************************************************************************
     Private functions.
     **************************************************************************************/
    private void addItems(RelativeLayout layout) {
        RelativeLayout myLayout = new RelativeLayout(this);
        myLayout.setBackgroundColor(Color.argb(255, 244, 244, 244));

        RelativeLayout.LayoutParams layoutParams = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        layoutParams.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        layout.addView(myLayout, layoutParams);

        this.addEditText(myLayout);
        this.addButton(myLayout);
    }

    private int dpToPixel(int dp) {
        final float scale = getResources().getDisplayMetrics().density;
        int px = (int) (dp * scale + 0.5f);
        return px;
    }
    private void addEditText(RelativeLayout layout) {
        mEditText = new Cocos2dxEditText(this);
        mEditText.setVisibility(View.INVISIBLE);
        mEditText.setGravity(Gravity.CENTER_VERTICAL);
        mEditText.setBackground(getRoundRectShape(18, Color.WHITE, Color.WHITE));
        mEditText.setId(mEditTextID);
        int bottomPadding = dpToPixel(4);
        int leftPadding = dpToPixel(3);
        mEditText.setPadding(leftPadding,bottomPadding,leftPadding,bottomPadding);

        RelativeLayout.LayoutParams editParams = new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);

        editParams.addRule(RelativeLayout.CENTER_VERTICAL);
        editParams.addRule(RelativeLayout.LEFT_OF, mButtonLayoutID);
        int bottomMargin = dpToPixel(5);
        int leftMargin = dpToPixel(4);
        editParams.setMargins(leftMargin, bottomMargin, bottomMargin, bottomMargin);
        layout.addView(mEditText, editParams);
    }

    private void addButton(RelativeLayout layout) {
        mButton = new Button(this);
        RelativeLayout.LayoutParams layoutParams = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        mButton.setTextColor(Color.WHITE);
        mButton.setTextSize(16);
        mButton.setBackground(getRoundRectShape(18, DARK_GREEN, DARK_GREEN_PRESS));
        int paddingLeft = dpToPixel(5);
        mButton.setPadding(paddingLeft,0,paddingLeft,0);
        layoutParams.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        layoutParams.addRule(RelativeLayout.ALIGN_TOP, mEditTextID);
        layoutParams.addRule(RelativeLayout.ALIGN_BOTTOM, mEditTextID);
        layoutParams.rightMargin = dpToPixel(4);
        layout.addView(mButton, layoutParams);
        mButton.setGravity(Gravity.CENTER);
        mButton.setId(mButtonLayoutID);

        mButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                CocosEditBoxActivity.this.onKeyboardConfirm(mEditText.getText().toString());

                if (!CocosEditBoxActivity.this.mConfirmHold)
                    CocosEditBoxActivity.this.hide();
            }
        });
    }

    private Drawable getRoundRectShape(int radius, int normalColor, int pressColor) {
        float[] outerRadii = new float[]{radius, radius, radius, radius, radius, radius, radius, radius};
        RoundRectShape roundRectShape = new RoundRectShape(outerRadii, null, null);
        ShapeDrawable shapeDrawableNormal = new ShapeDrawable();
        shapeDrawableNormal.setShape(roundRectShape);
        shapeDrawableNormal.getPaint().setStyle(Paint.Style.FILL);
        shapeDrawableNormal.getPaint().setColor(normalColor);
        ShapeDrawable shapeDrawablePress = new ShapeDrawable();
        shapeDrawablePress.setShape(roundRectShape);
        shapeDrawablePress.getPaint().setStyle(Paint.Style.FILL);
        shapeDrawablePress.getPaint().setColor(pressColor);
        StateListDrawable drawable = new StateListDrawable();
        drawable.addState(new int[]{android.R.attr.state_pressed}, shapeDrawablePress);
        drawable.addState(new int[]{}, shapeDrawableNormal);
        return drawable;
    }


    private void hide() {
        Utils.hideVirtualButton();
        this.closeKeyboard();
        finish();
    }

    public void show(String defaultValue, int maxLength, boolean isMultiline, boolean confirmHold, String confirmType, String inputType, boolean autocorrect) {
        mConfirmHold = confirmHold;
        mEditText.show(defaultValue, maxLength, isMultiline, confirmHold, confirmType, inputType, autocorrect);
        mButton.setText(mButtonTitle);
        if (TextUtils.isEmpty(mButtonTitle)) {
            mButton.setVisibility(View.INVISIBLE);
        } else {
            mButton.setVisibility(View.VISIBLE);
        }

        this.openKeyboard();
    }

    private void closeKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(this.INPUT_METHOD_SERVICE);
        imm.hideSoftInputFromWindow(mEditText.getWindowToken(), 0);

        this.onKeyboardComplete(mEditText.getText().toString());
    }

    private void openKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(this.INPUT_METHOD_SERVICE);
        imm.showSoftInput(mEditText, InputMethodManager.SHOW_IMPLICIT);
    }

    /***************************************************************************************
     Functions invoked by CPP.
     **************************************************************************************/

    private static void showNative(String defaultValue, int maxLength, boolean isMultiline, boolean confirmHold, String confirmType, String inputType, boolean autocorrect, boolean inputOnly) {

        GlobalObject.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (inputOnly) {
                    showInputOnly(defaultValue, maxLength, isMultiline, confirmHold, confirmType, inputType, autocorrect);
                    return;
                }
                Intent i = new Intent(GlobalObject.getActivity(), CocosEditBoxActivity.class);
                i.putExtra("defaultValue", defaultValue);
                i.putExtra("maxLength", maxLength);
                i.putExtra("isMultiline", isMultiline);
                i.putExtra("confirmHold", confirmHold);
                i.putExtra("confirmType", confirmType);
                i.putExtra("inputType", inputType);
                i.putExtra("autocorrect", autocorrect);
                i.putExtra(ORIENTATION, GlobalObject.getActivity().getRequestedOrientation());
                GlobalObject.getActivity().startActivity(i);
            }
        });
    }

    private static void hideNative() {
        if (sInputShown) {
            GlobalObject.runOnUiThread(CocosEditBoxActivity::hideInputOnly);
            return;
        }
        if (null != CocosEditBoxActivity.sThis) {
            GlobalObject.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    CocosEditBoxActivity.sThis.hide();
                }
            });
        }
    }

    /***************************************************************************************
     Native functions invoked by UI.
     **************************************************************************************/
    private void onKeyboardInput(String text) {
        CocosHelper.runOnGameThread(new Runnable() {
            @Override
            public void run() {
                CocosEditBoxActivity.onKeyboardInputNative(text);
            }
        });
    }

    private void onKeyboardComplete(String text) {
        CocosHelper.runOnGameThread(new Runnable() {
            @Override
            public void run() {
                CocosEditBoxActivity.onKeyboardCompleteNative(text);
            }
        });
    }

    private void onKeyboardConfirm(String text) {
        CocosHelper.runOnGameThread(new Runnable() {
            @Override
            public void run() {
                CocosEditBoxActivity.onKeyboardConfirmNative(text);
            }
        });
    }

    /***************************************************************************************
     Input-only editor.
     The game draws the field, caret and selection. A hidden EditText in the game's own activity takes the keyboard,
     so touches keep reaching the game, which decides when a tap ends editing, and the game keeps the text and
     selection in step through setTextNative. Editors without inputOnly use the activity above.
     **************************************************************************************/
    private static EditText sInput = null;
    private static volatile boolean sInputShown = false;
    private static boolean sInputKeyboardSeen = false;
    private static boolean sInputConfirmHold = false;
    private static boolean sInputMultiline = false;
    // Set while the game writes text and selection, so the change is not reported back to it.
    private static boolean sInputUpdating = false;
    private static volatile int sSelectionStart = -1;
    private static volatile int sSelectionEnd = -1;
    private static int sSavedSoftInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED;
    private static ViewTreeObserver.OnGlobalLayoutListener sKeyboardListener = null;

    private static void createInputOnly(Activity activity) {
        sInput = new EditText(activity) {
            @Override
            protected void onSelectionChanged(int selStart, int selEnd) {
                super.onSelectionChanged(selStart, selEnd);
                if (!sInputUpdating) {
                    sSelectionStart = selStart;
                    sSelectionEnd = selEnd;
                }
            }
        };
        sInput.setAlpha(0f);
        sInput.setBackground(null);
        sInput.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable s) {
                if (sInputUpdating || !sInputShown) return;
                final String text = s.toString();
                CocosHelper.runOnGameThread(() -> onKeyboardInputNative(text));
            }
        });
        sInput.setOnEditorActionListener((v, actionId, event) -> {
            if (sInputMultiline || !sInputShown) return false;
            // A hardware Enter reports both key down and key up.
            if (event != null && event.getAction() != KeyEvent.ACTION_DOWN) return true;
            final String text = sInput.getText().toString();
            CocosHelper.runOnGameThread(() -> onKeyboardConfirmNative(text));
            if (!sInputConfirmHold) hideInputOnly();
            return true;
        });
    }

    private static void showInputOnly(String defaultValue, int maxLength, boolean isMultiline, boolean confirmHold, String confirmType, String inputType, boolean autocorrect) {
        final Activity activity = GlobalObject.getActivity();
        if (sInput == null || sInput.getContext() != activity) createInputOnly(activity);
        if (sInput.getParent() == null) {
            // One pixel in the top-left corner: the keyboard pans nothing to reveal it, and no tap lands on it.
            ViewGroup root = activity.findViewById(android.R.id.content);
            root.addView(sInput, new FrameLayout.LayoutParams(1, 1, Gravity.TOP | Gravity.START));
        }
        sInputConfirmHold = confirmHold;
        sInputMultiline = isMultiline;
        sInputUpdating = true;
        sInput.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxLength)});
        sInput.setImeOptions(imeOptionsFor(confirmType));
        applyInputTraits(sInput, inputType, isMultiline, autocorrect);
        sInput.setText(defaultValue);
        sInput.setSelection(sInput.length());
        sInputUpdating = false;
        sSelectionStart = sSelectionEnd = sInput.length();

        final Window window = activity.getWindow();
        if (!sInputShown) {
            sSavedSoftInputMode = window.getAttributes().softInputMode;
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
        }
        sInputShown = true;
        sInputKeyboardSeen = false;
        if (sKeyboardListener == null) {
            // The user can close the keyboard (back, or the keyboard's own button); that ends editing.
            final View decor = window.getDecorView();
            sKeyboardListener = () -> {
                if (!sInputShown) return;
                Rect r = new Rect();
                decor.getWindowVisibleDisplayFrame(r);
                int height = decor.getRootView().getHeight();
                if (height - r.height() > height / 4) sInputKeyboardSeen = true;
                else if (sInputKeyboardSeen) hideInputOnly();
            };
            decor.getViewTreeObserver().addOnGlobalLayoutListener(sKeyboardListener);
        }

        sInput.setFocusable(true);
        sInput.setFocusableInTouchMode(true);
        sInput.requestFocus();
        final InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        // The input type may differ from the last session.
        imm.restartInput(sInput);
        sInput.post(() -> {
            if (sInputShown) imm.showSoftInput(sInput, 0);
        });
    }

    private static void hideInputOnly() {
        if (!sInputShown) return;
        sInputShown = false;
        sSelectionStart = sSelectionEnd = -1;
        Activity activity = (Activity) sInput.getContext();
        InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        imm.hideSoftInputFromWindow(sInput.getWindowToken(), 0);
        // Unfocusable, so focus does not return to it in touch mode.
        sInput.setFocusable(false);
        sInput.clearFocus();
        if (sKeyboardListener != null) {
            activity.getWindow().getDecorView().getViewTreeObserver().removeOnGlobalLayoutListener(sKeyboardListener);
            sKeyboardListener = null;
        }
        activity.getWindow().setSoftInputMode(sSavedSoftInputMode);
        Utils.hideVirtualButton();
        final String text = sInput.getText().toString();
        CocosHelper.runOnGameThread(() -> onKeyboardCompleteNative(text));
    }

    /** From the game thread: the game's text and selection, applied to the input-only editor. */
    private static void setTextNative(String text, int start, int end) {
        if (!sInputShown) return;
        final int from = Math.max(0, Math.min(Math.min(start, end), text.length()));
        final int to = Math.max(0, Math.min(Math.max(start, end), text.length()));
        // Reported at once: the game reads the selection back before the UI thread applies it.
        sSelectionStart = from;
        sSelectionEnd = to;
        GlobalObject.runOnUiThread(() -> {
            if (!sInputShown) return;
            sInputUpdating = true;
            // Replacing equal text would move the caret and break an IME composition.
            if (!sInput.getText().toString().equals(text)) sInput.setText(text);
            int length = sInput.length();
            int a = Math.min(from, length), b = Math.min(to, length);
            if (sInput.getSelectionStart() != a || sInput.getSelectionEnd() != b) sInput.setSelection(a, b);
            sInputUpdating = false;
        });
    }

    /** From the game thread: the input-only editor's selection, or -1 while it is not shown. */
    private static int getSelectionStartNative() { return sInputShown ? sSelectionStart : -1; }
    private static int getSelectionEndNative() { return sInputShown ? sSelectionEnd : -1; }

    private static native void onKeyboardInputNative(String text);
    private static native void onKeyboardCompleteNative(String text);
    private static native void onKeyboardConfirmNative(String text);
}
