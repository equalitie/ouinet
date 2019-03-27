package main

import (
	"context"
	"net"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"

	lampshade "github.com/getlantern/lampshade"
)

// #cgo CFLAGS: -DIN_GO=1 -ggdb -I ${SRCDIR}/../../include
// #cgo android LDFLAGS: -Wl,--unresolved-symbols=ignore-all
//#include <stdlib.h>
//#include <stddef.h>
//#include <stdint.h>
//#include <string.h>
//
//// Don't export these functions into C or we'll get "unused function" warnings
//// (Or errors saying functions are defined more than once if the're not static).
//
//#if IN_GO
//static void execute_cb_void(void* func, void* arg, int err)
//{
//    ((void(*)(void*, int)) func)(arg, err);
//}
//static void execute_cb_int(void* func, void* arg, int err, uint64_t data)
//{
//    ((void(*)(void*, int, uint64_t)) func)(arg, err, data);
//}
//static void execute_cb_buffers(void* func, void* arg, int err, void* data1, size_t size1, void* data2, size_t size2)
//{
//    ((void(*)(void*, int, void*, size_t, void*, size_t)) func)(arg, err, data1, size1, data2, size2);
//}
//#endif // if IN_GO
import "C"
import "unsafe"

func main() {
}

var g_buffer_pool lampshade.BufferPool = lampshade.NewBufferPool(128)

// This binding contains a bunch of object tables identified by ID, and
// assorted ID counters. All are only ever accessed by the binding API
// functions, and need external synchronization.


var g_cancellation_next_id uint64 = 0
var g_cancellations = make(map[uint64]func())

//export go_lampshade_cancellation_allocate
func go_lampshade_cancellation_allocate() uint64 {
	id := g_cancellation_next_id
	g_cancellation_next_id += 1
	return id
}

//export go_lampshade_cancellation_free
func go_lampshade_cancellation_free(cancellation_id uint64) {
	delete(g_cancellations, cancellation_id)
}

//export go_lampshade_cancellation_cancel
func go_lampshade_cancellation_cancel(cancellation_id uint64) {
	cancel, ok := g_cancellations[cancellation_id]
	if !ok { return }

	cancel()
}


type lampshade_Connection struct {
	connection net.Conn
}

var g_connection_next_id uint64 = 0
var g_connections = make(map[uint64]*lampshade_Connection)

//export go_lampshade_connection_allocate
func go_lampshade_connection_allocate() uint64 {
	var connection lampshade_Connection
	connection_id := g_connection_next_id
	g_connection_next_id += 1
	g_connections[connection_id] = &connection
	return connection_id
}

//export go_lampshade_connection_free
func go_lampshade_connection_free(connection_id uint64) {
	delete(g_connections, connection_id)
}

//export go_lampshade_connection_send
func go_lampshade_connection_send(connection_id uint64, buffer unsafe.Pointer, buffer_size C.int, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	connection, ok := g_connections[connection_id]
	if !ok {
		C.execute_cb_int(callback, callback_arg, 1, 0)
		return
	}

	if connection.connection == nil {
		C.execute_cb_int(callback, callback_arg, 1, 0)
		return
	}

	data := C.GoBytes(buffer, buffer_size)

	go func() {
		written, err := connection.connection.Write(data)

		if err != nil {
			C.execute_cb_int(callback, callback_arg, 1, 0)
			return
		}

		C.execute_cb_int(callback, callback_arg, 0, C.uint64_t(written))
	}()
}

//export go_lampshade_connection_receive
func go_lampshade_connection_receive(connection_id uint64, buffer unsafe.Pointer, buffer_size C.int, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	connection, ok := g_connections[connection_id]
	if !ok {
		C.execute_cb_int(callback, callback_arg, 1, 0)
		return
	}

	if connection.connection == nil {
		C.execute_cb_int(callback, callback_arg, 1, 0)
		return
	}

	go func() {
		read_buffer := make([]byte, buffer_size)
		read, err := connection.connection.Read(read_buffer)

		if err != nil {
			C.execute_cb_int(callback, callback_arg, 1, 0)
			return
		}

		C.memcpy(buffer, unsafe.Pointer(&read_buffer[0]), C.size_t(read))

		C.execute_cb_int(callback, callback_arg, 0, C.uint64_t(read))
	}()
}

//export go_lampshade_connection_close
func go_lampshade_connection_close(connection_id uint64, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	connection, ok := g_connections[connection_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if connection.connection == nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	go func() {
		err := connection.connection.Close()

		if err != nil {
			C.execute_cb_void(callback, callback_arg, 1)
			return
		}

		C.execute_cb_void(callback, callback_arg, 0)
	}()
}


type lampshade_Dialer struct {
	dialer lampshade.BoundDialer
}

var g_dialer_next_id uint64 = 0
var g_dialers = make(map[uint64]*lampshade_Dialer)

//export go_lampshade_dialer_allocate
func go_lampshade_dialer_allocate() uint64 {
	var dialer lampshade_Dialer
	dialer_id := g_dialer_next_id
	g_dialer_next_id += 1
	g_dialers[dialer_id] = &dialer
	return dialer_id
}

//export go_lampshade_dialer_free
func go_lampshade_dialer_free(dialer_id uint64) {
	delete(g_dialers, dialer_id)
}

//export go_lampshade_dialer_init
func go_lampshade_dialer_init(dialer_id uint64, c_endpoint *C.char, public_key_buffer unsafe.Pointer, public_key_size C.int, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	endpoint := C.GoString(c_endpoint)
	encoded_public_key := C.GoBytes(public_key_buffer, public_key_size)

	public_key, err := x509.ParsePKCS1PublicKey(encoded_public_key)

	if err != nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	dialer, ok := g_dialers[dialer_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if dialer.dialer != nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	go func() {
		dialer.dialer = lampshade.NewDialer(&lampshade.DialerOpts{
			Pool: g_buffer_pool,
			Cipher: lampshade.AES128GCM,
			ServerPublicKey: public_key,
		}).BoundTo(func() (net.Conn, error) {
			return net.Dial("tcp", endpoint)
		})

		C.execute_cb_void(callback, callback_arg, 0)
	}()
}

//export go_lampshade_dialer_dial
func go_lampshade_dialer_dial(dialer_id uint64, connection_id uint64, cancellation_id uint64, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	dialer, ok := g_dialers[dialer_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if dialer.dialer == nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	connection, ok := g_connections[connection_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if connection.connection != nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	cancel_ctx, cancel_func := context.WithCancel(context.Background())
	g_cancellations[cancellation_id] = cancel_func

	go func() {
		conn, err := dialer.dialer.DialContext(cancel_ctx)

		if err != nil {
			C.execute_cb_void(callback, callback_arg, 1)
			return
		}

		connection.connection = conn
		C.execute_cb_void(callback, callback_arg, 0)
	}()
}


type lampshade_Listener struct {
	listener net.Listener
}

var g_listener_next_id uint64 = 0
var g_listeners = make(map[uint64]*lampshade_Listener)

//export go_lampshade_listener_allocate
func go_lampshade_listener_allocate() uint64 {
	var listener lampshade_Listener
	listener_id := g_listener_next_id
	g_listener_next_id += 1
	g_listeners[listener_id] = &listener
	return listener_id
}

//export go_lampshade_listener_free
func go_lampshade_listener_free(listener_id uint64) {
	delete(g_listeners, listener_id)
}

//export go_lampshade_listener_create
func go_lampshade_listener_create(listener_id uint64, c_endpoint *C.char, private_key_buffer unsafe.Pointer, private_key_size C.int, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	endpoint := C.GoString(c_endpoint)
	encoded_private_key := C.GoBytes(private_key_buffer, private_key_size)

	private_key, err := x509.ParsePKCS1PrivateKey(encoded_private_key)

	if err != nil {
		C.execute_cb_void(callback, callback_arg, 1)
	}

	listener, ok := g_listeners[listener_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if listener.listener != nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	go func() {
		tcp_listener, err := net.Listen("tcp", endpoint)
		if err != nil {
			C.execute_cb_void(callback, callback_arg, 1)
			return
		}

		listener.listener = lampshade.WrapListener(tcp_listener, g_buffer_pool, private_key, true)

		C.execute_cb_void(callback, callback_arg, 0)
	}()
}

//export go_lampshade_listener_accept
func go_lampshade_listener_accept(listener_id uint64, connection_id uint64, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	listener, ok := g_listeners[listener_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if listener.listener == nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	connection, ok := g_connections[connection_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if connection.connection != nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	go func() {
		conn, err := listener.listener.Accept()

		if err != nil {
			C.execute_cb_void(callback, callback_arg, 1)
			return
		}

		connection.connection = conn
		C.execute_cb_void(callback, callback_arg, 0)
	}()
}

//export go_lampshade_listener_close
func go_lampshade_listener_close(listener_id uint64, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	listener, ok := g_listeners[listener_id]
	if !ok {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	if listener.listener == nil {
		C.execute_cb_void(callback, callback_arg, 1)
		return
	}

	go func() {
		err := listener.listener.Close()

		if err != nil {
			C.execute_cb_void(callback, callback_arg, 1)
			return
		}

		C.execute_cb_void(callback, callback_arg, 0)
	}()
}


//export go_lampshade_generate_key
func go_lampshade_generate_key(bits int, callback unsafe.Pointer, callback_arg unsafe.Pointer) {
	private_key, err := rsa.GenerateKey(rand.Reader, bits)
	if err != nil {
		C.execute_cb_buffers(callback, callback_arg, 1, nil, 0, nil, 0)
		return
	}

	encoded_private_key := x509.MarshalPKCS1PrivateKey(private_key)
	encoded_public_key := x509.MarshalPKCS1PublicKey(&private_key.PublicKey)

	c_private_key := C.CBytes(encoded_private_key)
	c_private_key_size := C.size_t(len(encoded_private_key))
	defer C.free(c_private_key)
	c_public_key := C.CBytes(encoded_public_key)
	c_public_key_size := C.size_t(len(encoded_public_key))
	defer C.free(c_public_key)

	C.execute_cb_buffers(callback, callback_arg, 0, c_private_key, c_private_key_size, c_public_key, c_public_key_size)
}

