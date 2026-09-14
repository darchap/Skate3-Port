package io.skate3port.game;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

/**
 * Keeps the game process out of the cached-app kill bucket while a session is
 * live: the 8 GiB guest mapping makes a cached process the first LMK victim.
 */
public class GameKeepAliveService extends Service {
    private static final String CHANNEL_ID = "skate3_session";
    private static final int NOTIFICATION_ID = 1;

    static void start(Context context) {
        context.startForegroundService(new Intent(context, GameKeepAliveService.class));
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, GameKeepAliveService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Game session", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps the game session alive while in the background");
        getSystemService(NotificationManager.class).createNotificationChannel(channel);

        PendingIntent tapIntent = PendingIntent.getActivity(this, 0,
                new Intent(this, Skate3Activity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE);
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle("Skate 3 session active")
                .setContentText("Tap to return to the game")
                .setContentIntent(tapIntent)
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION_ID, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
