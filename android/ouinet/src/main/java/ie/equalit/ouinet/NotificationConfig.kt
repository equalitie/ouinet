package ie.equalit.ouinet

import android.content.Context
import android.os.Parcel
import android.os.Parcelable
import android.os.Parcelable.Creator
import kotlin.properties.Delegates

/**
 * Contains the configuration settings for the Ouinet foreground notification.
 * The config is immutable and can be passed around as a Parcelable.
 * The Config has a private constructor and should only be created by the ConfigBuilder,
 * This should only be done once in the life time of the app to avoid race conditions.
 */
class NotificationConfig : Parcelable {
    class Builder(context: Context) {
        private val context : Context
        private var activityName : String? = null
        private var statusIcon by Delegates.notNull<Int>()
        private var homeIcon : Int
        private var clearIcon : Int
        private var channelName : String
        private var title : String
        private var description : String
        private var homeText : String
        private var clearText : String
        private var confirmText : String
        private var updateInterval : Long
        private var disableStatus : Boolean

        init {
            this.context = context
            this.homeIcon = R.drawable.ouinet_globe_pm
            this.clearIcon = R.drawable.ouinet_cancel_pm
            this.channelName = context.getString(R.string.ouinet_notif_channel)
            this.title = context.getString(R.string.ouinet_notif_title)
            this.description = context.getString(R.string.ouinet_notif_detail)
            this.homeText = context.getString(R.string.ouinet_notif_home)
            this.clearText = context.getString(R.string.ouinet_notif_clear)
            this.confirmText = context.getString(R.string.ouinet_notif_confirm)
            this.updateInterval = OuinetNotification.DEFAULT_INTERVAL
            this.disableStatus = false
        }

        /* Sets the activity to be started upon selecting the "Home" option from
         * the ouinet notification, takes the fully-qualified class name of
         * the activity to be started as a String.
         */
        fun setHomeActivity(
            activityName: String?
        ): Builder {
            this.activityName = activityName
            return this
        }

        // TODO: status icon needs a default
        @JvmOverloads
        fun setNotificationIcons(
            statusIcon: Int,
            homeIcon : Int = this.homeIcon,
            clearIcon : Int = this.clearIcon,
        ): Builder {
            this.statusIcon = statusIcon
            this.homeIcon = homeIcon
            this.clearIcon = clearIcon
            return this
        }

        fun setChannelName(
            channelName: String
        ): Builder {
            this.channelName = channelName
            return this
        }

        @JvmOverloads
        fun setNotificationText(
            title: String = this.title,
            description : String = this.description,
            homeText : String = this.homeText,
            clearText : String = this.clearText,
            confirmText : String = this.confirmText
        ): Builder {
            this.title = title
            this.description = description
            this.homeText = homeText
            this.clearText = clearText
            this.confirmText = confirmText
            return this
        }

        /*
         * Sets how often the ouinet state should be updated in the notification text
         * Defaults to 5s
         */
        fun setUpdateInterval(
            updateInterval : Long
        ) : Builder {
            this.updateInterval = updateInterval
            return this
        }

        fun disableStatusUpdate(
            disableStatus : Boolean
        ) : Builder {
            this.disableStatus = disableStatus
            return this
        }

        fun build() = NotificationConfig(
                activityName,
                statusIcon,
                homeIcon,
                clearIcon,
                channelName,
                title,
                description,
                homeText,
                clearText,
                confirmText,
                updateInterval,
                disableStatus
            )
    }

    var activityName: String?
        private set
    var statusIcon: Int
        private set
    var homeIcon: Int
        private set
    var clearIcon: Int
        private set
    var channelName: String?
        private set
    var title: String?
        private set
    var description: String?
        private set
    var homeText: String?
        private set
    var clearText: String?
        private set
    var confirmText: String?
        private set
    var updateInterval: Long
        private set
    var disableStatus: Boolean
        private set

    private constructor(
        activityName: String?,
        statusIcon: Int,
        homeIcon: Int,
        clearIcon: Int,
        channelName: String?,
        title: String?,
        description: String?,
        homeText: String?,
        clearText: String?,
        confirmText: String?,
        updateInterval: Long,
        disableStatus: Boolean
    ) {
        this.activityName = activityName
        this.statusIcon = statusIcon
        this.homeIcon = homeIcon
        this.clearIcon = clearIcon
        this.channelName = channelName
        this.title = title
        this.description = description
        this.homeText = homeText
        this.clearText = clearText
        this.confirmText = confirmText
        this.updateInterval = updateInterval
        this.disableStatus = disableStatus
    }

    override fun describeContents(): Int {
        return 0
    }

    override fun writeToParcel(out: Parcel, flags: Int) {
        out.writeString(activityName)
        out.writeInt(statusIcon)
        out.writeInt(homeIcon)
        out.writeInt(clearIcon)
        out.writeString(channelName)
        out.writeString(title)
        out.writeString(description)
        out.writeString(homeText)
        out.writeString(clearText)
        out.writeString(confirmText)
        out.writeLong(updateInterval)
        val disable = if (disableStatus) 1 else 0
        out.writeInt(disable)
    }

    private constructor(`in`: Parcel) {
        activityName = `in`.readString()
        statusIcon = `in`.readInt()
        homeIcon = `in`.readInt()
        clearIcon = `in`.readInt()
        channelName = `in`.readString()
        title = `in`.readString()
        description = `in`.readString()
        homeText = `in`.readString()
        clearText = `in`.readString()
        confirmText = `in`.readString()
        updateInterval = `in`.readLong()
        val disable = `in`.readInt()
        disableStatus = (disable == 1)
    }

    companion object {
        @JvmField
        val CREATOR: Creator<NotificationConfig?> = object : Creator<NotificationConfig?> {
            override fun createFromParcel(`in`: Parcel): NotificationConfig {
                return NotificationConfig(`in`)
            }

            override fun newArray(size: Int): Array<NotificationConfig?> {
                return arrayOfNulls(size)
            }
        }
    }
}
