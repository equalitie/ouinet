package ie.equalit.ouinet

class OuinetEndpoint(endpoint: String) {
    private val address: String
    private val port: Int

    init {
        val parts = endpoint.split(":")
        require(parts.size == 2) { "Invalid endpoint format." }

        address = parts[0]
        port = parts[1].toIntOrNull() ?: throw IllegalArgumentException("Port must be a valid integer.")

        require(port in 1..65535) { "Port must be between 1 and 65535." }
    }

    fun getAddress(): String {
        return address
    }

    fun getPort(): Int {
        return port
    }

    override fun toString(): String {
        return "$address:$port"
    }
}