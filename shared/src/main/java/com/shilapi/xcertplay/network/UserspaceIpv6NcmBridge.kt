package com.shilapi.xcertplay.network

import android.util.Log
import com.shilapi.xcertplay.transport.NcmUsbBridge
import java.io.Closeable
import java.net.InetAddress
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Userspace IPv6/TCP transport over the iPhone NCM link for head units whose kernel
 * refuses IPv6 on a VpnService tun (Li Auto: disable_ipv6=1).
 *
 * lwIP terminates TCP from the phone; each accepted connection is relayed to the local
 * AirPlay listener, so AirPlaySession keeps using an ordinary java.net.Socket.
 */
class UserspaceIpv6NcmBridge(
    private val ncm: NcmUsbBridge,
    private val hostMac: ByteArray,
    private val port: Int,
    private val onError: (Throwable) -> Unit,
) : Closeable {

    init { require(hostMac.size == 6) { "hostMac must be 6 bytes" } }

    private val running = AtomicBoolean(false)
    private var inThread: Thread? = null
    private var tickThread: Thread? = null
    private val relays = ConcurrentHashMap<Int, Relay>()
    private val udpSocks = ConcurrentHashMap<Int, java.net.DatagramSocket>()
    private val udpPeers = ConcurrentHashMap<Int, Int>()

    @Volatile private var airPlayPort: Int = port

    companion object {
        private const val TAG = "UserspaceIPv6Bridge"
        init { System.loadLibrary("userspace_network_stack") }
    }

    /** Actual port the AirPlay server bound; called by CarPlayVpnService after bind. */
    fun setAirPlayPort(bound: Int) {
        airPlayPort = bound
        Log.i(TAG, "relay target port=$bound")
    }

    fun start() {
        check(running.compareAndSet(false, true)) { "bridge already started" }
        nativeSetCallback(Callback())
        val rc = nativeInit(hostMac, port)
        if (rc != 0) {
            running.set(false)
            onError(RuntimeException("lwIP init failed: $rc"))
            return
        }
        inThread = Thread({ pumpInbound() }, "ncm-us-in").apply { isDaemon = true; start() }
        tickThread = Thread({ pumpTimers() }, "ncm-us-tick").apply { isDaemon = true; start() }
        Thread({ pumpUdp() }, "ncm-us-udp").apply { isDaemon = true; start() }
        Log.i(TAG, "userspace IPv6 bridge started on port $port")
    }

    override fun close() {
        if (!running.compareAndSet(true, false)) return
        relays.values.forEach { runCatching { it.close() } }
        relays.clear()
        runCatching { nativeShutdown() }
        runCatching { nativeSetCallback(null) }
        listOf(inThread, tickThread).forEach { t ->
            t?.interrupt(); runCatching { t?.join(2000) }
        }
        inThread = null; tickThread = null
        runCatching { ncm.close() }
    }

    private fun pumpInbound() {
        try {
            while (running.get()) {
                val frame = ncm.recv(500) ?: continue
                nativeInput(frame)
            }
        } catch (e: Exception) { if (running.get()) onError(e) }
    }

    private fun pumpTimers() {
        try {
            while (running.get()) { nativeTick(); Thread.sleep(50) }
        } catch (_: InterruptedException) {
        } catch (e: Exception) { if (running.get()) onError(e) }
    }

    /** One lwIP connection <-> one local socket to the AirPlay listener. */
    private inner class Relay(val id: Int, targetPort: Int) {
        private val socket = Socket(InetAddress.getByName("127.0.0.1"), targetPort)
        private val out = socket.getOutputStream()
        private val alive = AtomicBoolean(true)
        private val reader: Thread

        init {
            socket.tcpNoDelay = true
            socket.keepAlive = true
            reader = Thread({ pumpToStack() }, "ncm-relay-$id").apply { isDaemon = true; start() }
        }

        /** phone -> AirPlay */
        fun toAirPlay(data: ByteArray) {
            if (!alive.get()) return
            try { out.write(data); out.flush() } catch (e: Exception) { close() }
        }

        /** AirPlay -> phone */
        private fun pumpToStack() {
            val buf = ByteArray(16 * 1024)
            try {
                val input = socket.getInputStream()
                while (alive.get() && running.get()) {
                    val n = input.read(buf)
                    if (n < 0) break
                    var off = 0
                    while (off < n && alive.get()) {
                        val chunk = if (n - off > 4096) buf.copyOfRange(off, off + 4096)
                                    else buf.copyOfRange(off, n)
                        val written = nativeWrite(id, chunk)
                        if (written < 0) { alive.set(false); break }
                        if (written == 0) { Thread.sleep(5); continue }
                        off += written
                    }
                }
            } catch (_: Exception) {
            } finally { close() }
        }

        fun close() {
            if (!alive.compareAndSet(true, false)) return
            runCatching { socket.close() }
            relays.remove(id)
            runCatching { nativeCloseConn(id) }
        }
    }

    private fun pumpUdp() {
        MicUplink.send = { l, r, d -> nativeUdpSend(l, r, d) }
        MicUplink.localPorts = { udpPeers.keys.toList() }
        val buf = ByteArray(2048)
        while (running.get()) {
            try {
                for ((port, sock) in udpSocks) {
                    if (!sock.isBound) continue
                    sock.soTimeout = 20
                    val pkt = java.net.DatagramPacket(buf, buf.size)
                    try { sock.receive(pkt) } catch (_: java.net.SocketTimeoutException) { continue }
                    val peer = udpPeers[port] ?: continue
                    nativeUdpSend(port, peer, pkt.data.copyOf(pkt.length))
                }
                Thread.sleep(5)
            } catch (_: InterruptedException) { return }
            catch (_: Exception) {}
        }
    }

    @Suppress("unused")
    private inner class Callback {
        fun onFrameOutput(frame: ByteArray) {
            try { ncm.send(frame, 2000) } catch (e: Exception) { if (running.get()) onError(e) }
        }

        fun onAccept(id: Int, localPort: Int) {
            val target = if (localPort == port) airPlayPort else localPort
            Log.i(TAG, "accept id=$id ncmPort=$localPort -> 127.0.0.1:$target")
            try { relays[id] = Relay(id, target) }
            catch (e: Exception) {
                Log.e(TAG, "relay connect failed", e)
                runCatching { nativeCloseConn(id) }
            }
        }

        fun onData(id: Int, data: ByteArray) { relays[id]?.toAirPlay(data) }

        fun onClosed(id: Int) { relays.remove(id)?.close() }

        fun onUdpData(localPort: Int, remotePort: Int, data: ByteArray) {
            udpPeers[localPort] = remotePort
            try {
                val sock = udpSocks.getOrPut(localPort) { java.net.DatagramSocket() }
                sock.send(java.net.DatagramPacket(data, data.size,
                    InetAddress.getByName("127.0.0.1"), localPort))
            } catch (_: Exception) {}
        }
    }

    private external fun nativeInit(hostMac: ByteArray, port: Int): Int
    private external fun nativeInput(frame: ByteArray)
    private external fun nativeWrite(id: Int, data: ByteArray): Int
    private external fun nativeCloseConn(id: Int)
    private external fun nativeTick()
    private external fun nativeSetCallback(callback: Callback?)
    private external fun nativeShutdown()
    private external fun nativeUdpSend(localPort: Int, remotePort: Int, data: ByteArray): Int
}


/**
 * The microphone uplink is a new destination, not a reply: DiPlay sends it to the control peer's
 * address, which over this bridge is loopback, at the port the iPhone named in SETUP. Nothing
 * listens there, so the packets are dropped. Bind that port here and hand them to the IPv6 stack.
 */
object MicUplink {
    @Volatile var send: ((Int, Int, ByteArray) -> Int)? = null
    @Volatile var localPorts: (() -> List<Int>)? = null
    private var socket: java.net.DatagramSocket? = null
    private var boundPort = 0
    private var logged = false

    @Synchronized
    fun listen(port: Int) {
        if (port == boundPort && socket?.isClosed == false) return
        runCatching { socket?.close() }
        socket = null; boundPort = 0; logged = false
        val s = runCatching {
            java.net.DatagramSocket(null).apply {
                reuseAddress = true
                bind(java.net.InetSocketAddress(java.net.InetAddress.getByName("127.0.0.1"), port))
            }
        }.getOrElse {
            android.util.Log.w("UserspaceIPv6Bridge", "mic uplink bind failed port=$port", it); return
        }
        socket = s; boundPort = port
        android.util.Log.i("UserspaceIPv6Bridge", "mic uplink listening 127.0.0.1:$port")
        Thread({
            val buf = ByteArray(2048)
            while (!s.isClosed) {
                val pkt = java.net.DatagramPacket(buf, buf.size)
                try { s.receive(pkt) } catch (_: Exception) { break }
                val data = pkt.data.copyOf(pkt.length)
                val fn = send ?: continue
                var used = port
                var r = runCatching { fn(port, port, data) }.getOrDefault(-1)
                if (r < 0) {
                    for (lp in localPorts?.invoke().orEmpty()) {
                        r = runCatching { fn(lp, port, data) }.getOrDefault(-1)
                        if (r >= 0) { used = lp; break }
                    }
                }
                if (!logged && r >= 0) {
                    logged = true
                    android.util.Log.i("UserspaceIPv6Bridge",
                        "mic uplink forwarded bytes=${data.size} localPort=$used phonePort=$port")
                }
            }
        }, "mic-uplink").apply { isDaemon = true; start() }
    }
}
