/*
 * Network layer for MPlayer
 * by Bertrand BAUDET <bertrand_baudet@yahoo.com>
 * (C) 2001, MPlayer team.
 */

//#define DUMP2FILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <ctype.h>

#include "config.h"

#include "stream.h"
#include "demuxer.h"

#include "network.h"
#include "http.h"
#include "url.h"
#include "asf.h"
#include "rtp.h"

static struct {
	char *mime_type;
	int demuxer_type;
} mime_type_table[] = {
	// MP3 streaming, some MP3 streaming server answer with audio/mpeg
	{ "audio/mpeg", DEMUXER_TYPE_MPEG_PS },
	// MPEG streaming
	{ "video/mpeg", DEMUXER_TYPE_MPEG_PS },
	// AVI ??? => video/x-msvideo
	{ "video/x-msvideo", DEMUXER_TYPE_AVI },
	// MOV => video/quicktime
	{ "video/quicktime", DEMUXER_TYPE_MOV },
	// ASF
        { "audio/x-ms-wax", DEMUXER_TYPE_ASF },
	{ "audio/x-ms-wma", DEMUXER_TYPE_ASF },
	{ "video/x-ms-asf", DEMUXER_TYPE_ASF },
	{ "video/x-ms-afs", DEMUXER_TYPE_ASF },
	{ "video/x-ms-wvx", DEMUXER_TYPE_ASF },
	{ "video/x-ms-wmv", DEMUXER_TYPE_ASF },
	{ "video/x-ms-wma", DEMUXER_TYPE_ASF },
};

static struct {
	char *extension;
	int demuxer_type;
} extensions_table[] = {
	{ "mpeg", DEMUXER_TYPE_MPEG_PS },
	{ "mpg", DEMUXER_TYPE_MPEG_PS },
	{ "mpe", DEMUXER_TYPE_MPEG_ES },
	{ "avi", DEMUXER_TYPE_AVI },
	{ "mov", DEMUXER_TYPE_MOV },
	{ "qt", DEMUXER_TYPE_MOV },
	{ "asx", DEMUXER_TYPE_ASF },
	{ "asf", DEMUXER_TYPE_ASF },
	{ "wmv", DEMUXER_TYPE_ASF },
	{ "wma", DEMUXER_TYPE_ASF },
	{ "viv", DEMUXER_TYPE_VIVO },
};

streaming_ctrl_t *
streaming_ctrl_new( ) {
	streaming_ctrl_t *streaming_ctrl;
	streaming_ctrl = (streaming_ctrl_t*)malloc(sizeof(streaming_ctrl_t));
	if( streaming_ctrl==NULL ) {
		printf("Failed to allocate memory\n");
		return NULL;
	}
	memset( streaming_ctrl, 0, sizeof(streaming_ctrl_t) );
	return streaming_ctrl;
}

void
streaming_ctrl_free( streaming_ctrl_t *streaming_ctrl ) {
	if( streaming_ctrl==NULL ) return;
	free( streaming_ctrl );
}

int
read_rtp_from_server(int fd, char *buffer, int length) {
	int ret;
	int done=0;
	fd_set set;
	struct timeval tv;
	struct rtpheader rh;
	char *data;
	int len;
	static int got_first = 0;
	static int sequence;

	if( buffer==NULL || length<0 ) return -1;

	getrtp2(fd, &rh, &data, &len);
	if( got_first && rh.b.sequence != sequence+1 )
		printf("RTP packet sequence error!  Expected: %d, received: %d\n", 
			sequence+1, rh.b.sequence);
	got_first = 1;
	sequence = rh.b.sequence;
	memcpy(buffer, data, len);
	return(len);
}

// Connect to a server using a TCP connection
int
connect2Server(char *host, int port) {
	int socket_server_fd;
	int err, err_len;
	int ret;
	fd_set set;
	struct timeval tv;
	struct sockaddr_in server_address;

	printf("Connecting to server %s:%d ...\n", host, port );

	socket_server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if( socket_server_fd==-1 ) {
		perror("Failed to create socket");
		return -1;
	}

	if( isalpha(host[0]) ) {
		struct hostent *hp;
		hp=(struct hostent*)gethostbyname( host );
		if( hp==NULL ) {
			printf("Counldn't resolve name: %s\n", host);
			return -1;
		}
		memcpy( (void*)&server_address.sin_addr.s_addr, (void*)hp->h_addr, hp->h_length );
	} else {
		inet_pton(AF_INET, host, &server_address.sin_addr);
	}
	server_address.sin_family=AF_INET;
	server_address.sin_port=htons(port);
	
	// Turn the socket as non blocking so we can timeout on the connection
	fcntl( socket_server_fd, F_SETFL, fcntl(socket_server_fd, F_GETFL) | O_NONBLOCK );
	if( connect( socket_server_fd, (struct sockaddr*)&server_address, sizeof(server_address) )==-1 ) {
		if( errno!=EINPROGRESS ) {
			perror("Failed to connect to server");
			close(socket_server_fd);
			return -1;
		}
	}
	tv.tv_sec = 5;	// 5 seconds timeout on connection
	tv.tv_usec = 0;	
	FD_ZERO( &set );
	FD_SET( socket_server_fd, &set );
	// When the connection will be made, we will have a writable fd
	ret = select(socket_server_fd+1, NULL, &set, NULL, &tv);
	if( ret<=0 ) {
		if( ret<0 ) perror("select failed");
		else printf("Connection timeout\n");
		return -1;
	}

	// Turn back the socket as blocking
	fcntl( socket_server_fd, F_SETFL, fcntl(socket_server_fd, F_GETFL) & ~O_NONBLOCK );
	// Check if there were any error
	err_len = sizeof(int);
	ret =  getsockopt(socket_server_fd,SOL_SOCKET,SO_ERROR,&err,&err_len);
	if(ret < 0) {
		printf("getsockopt failed : %s\n",strerror(errno));
		return -1;
	}
	if(err > 0) {
		printf("Connect error : %s\n",strerror(err));
		return -1;
	}
	return socket_server_fd;
}

int
http_send_request( URL_t *url ) {
	HTTP_header_t *http_hdr;
	char str[80];
	int fd;
	http_hdr = http_new_header();
	http_set_uri( http_hdr, url->file );
	snprintf(str, 80, "Host: %s", url->hostname );
	http_set_field( http_hdr, str);
	http_set_field( http_hdr, "User-Agent: MPlayer");
	http_set_field( http_hdr, "Connection: closed");
	if( http_build_request( http_hdr )==NULL ) {
		return -1;
	}

	if( url->port==0 ) url->port = 80;
	fd = connect2Server( url->hostname, url->port );
	if( fd<0 ) {
		return -1; 
	}
	write( fd, http_hdr->buffer, http_hdr->buffer_size );
	http_free( http_hdr );

	return fd;
}

HTTP_header_t *
http_read_response( int fd ) {
	HTTP_header_t *http_hdr;
	char response[BUFFER_SIZE];
	int i;

	http_hdr = http_new_header();
	if( http_hdr==NULL ) {
		return NULL;
	}

	do {
		i = read( fd, response, BUFFER_SIZE ); 
		if( i<0 ) {
			printf("Read failed\n");
			http_free( http_hdr );
			return NULL;
		}
		if( i==0 ) {
			printf("http_read_response read 0 -ie- EOF\n");
			http_free( http_hdr );
			return NULL;
		}
		http_response_append( http_hdr, response, i );
	} while( !http_is_header_entire( http_hdr ) ); 
	http_response_parse( http_hdr );
	return http_hdr;
}

// By using the protocol, the extension of the file or the content-type
// we might be able to guess the streaming type.
int
autodetectProtocol(URL_t *url, int *fd_out) {
	HTTP_header_t *http_hdr;
	int fd=-1;
	int i;
	int redirect;
	char *extension;
	char *content_type;
	char *next_url;
	char response[1024];

	do {
		*fd_out=-1;
		next_url = NULL;
		extension = NULL;
		content_type = NULL;
		redirect = 0;

		if( url==NULL ) return DEMUXER_TYPE_UNKNOWN;

		// Get the extension of the file if present
		if( url->file!=NULL ) {
			for( i=strlen(url->file) ; i>0 ; i-- ) {
				if( url->file[i]=='.' ) {
					extension=(url->file)+i+1;
					break;
				}
			}
		}
extension=NULL;	
		if( extension!=NULL ) {
			printf("Extension: %s\n", extension );
			// Look for the extension in the extensions table
			for( i=0 ; i<(sizeof(extensions_table)/sizeof(extensions_table[0])) ; i++ ) {
				if( !strcasecmp(extension, extensions_table[i].extension) ) {
					//if( url->port==0 ) url->port = 80;
					return extensions_table[i].demuxer_type;
				}
			}
		}

		// Checking for RTSP
		if( !strcasecmp(url->protocol, "rtsp") ) {
			printf("RTSP protocol not yet implemented!\n");
			return DEMUXER_TYPE_UNKNOWN;
		}

		// Checking for RTP
		if( !strcasecmp(url->protocol, "rtp") ) {
			if( url->port==0 )
			{
				printf("You must enter a port number for RTP streams!\n");
				exit(1);	//fixme
			}
			*fd_out=-1;
			return DEMUXER_TYPE_UNKNOWN;
		}

		// Checking for ASF
		if( !strncasecmp(url->protocol, "mms", 3) ) {
			//if( url->port==0 ) url->port = 80;
			return DEMUXER_TYPE_ASF;
		}

		// HTTP based protocol
		if( !strcasecmp(url->protocol, "http") ) {
			if( url->port==0 ) url->port = 80;

			fd = http_send_request( url );
			if( fd<0 ) {
				*fd_out=-1;
				return DEMUXER_TYPE_UNKNOWN;
			}

			http_hdr = http_read_response( fd );
			if( http_hdr==NULL ) {
				close( fd );
				*fd_out=-1;
				http_free( http_hdr );
				return DEMUXER_TYPE_UNKNOWN;
			}

			*fd_out=fd;
			http_debug_hdr( http_hdr );

			// Check if the response is an ICY status_code reason_phrase
			if( !strcasecmp(http_hdr->protocol, "ICY") ) {
				// Ok, we have detected an mp3 streaming
				http_free( http_hdr );
				return DEMUXER_TYPE_MPEG_PS;
			}
			
			switch( http_hdr->status_code ) {
				case 200: // OK
					// Look if we can use the Content-Type
					content_type = http_get_field( http_hdr, "Content-Type" );
					if( content_type!=NULL ) {
						printf("Content-Type: [%s]\n", content_type );
						printf("Content-Length: [%s]\n", http_get_field(http_hdr, "Content-Length") );
						// Check in the mime type table for a demuxer type
						for( i=0 ; i<(sizeof(mime_type_table)/sizeof(mime_type_table[0])) ; i++ ) {
							if( !strcasecmp( content_type, mime_type_table[i].mime_type ) ) {
								http_free( http_hdr );
								return mime_type_table[i].demuxer_type;
							}
						}
					}
					break;
				// Redirect
				case 301: // Permanently
				case 302: // Temporarily
					// TODO: RFC 2616, recommand to detect infinite redirection loops
					next_url = http_get_field( http_hdr, "Location" );
					if( next_url!=NULL ) {
						close( fd );
						url_free( url );
						url = url_new( next_url );
						redirect = 1;	
					}
					break;
				default:
					printf("Server returned %d: %s\n", http_hdr->status_code, http_hdr->reason_phrase );
					close( fd );
					*fd_out=-1;
					http_free( http_hdr );
					return DEMUXER_TYPE_UNKNOWN;
			}
		} else {
			printf("Unknown protocol '%s'\n", url->protocol );
			return DEMUXER_TYPE_UNKNOWN;
		}
	} while( redirect );

	http_free( http_hdr );
	return DEMUXER_TYPE_UNKNOWN;
}

int
streaming_bufferize( streaming_ctrl_t *streaming_ctrl, char *buffer, int size) {
printf("streaming_bufferize\n");
	streaming_ctrl->buffer = (char*)malloc(size);
	if( streaming_ctrl->buffer==NULL ) {
		printf("Memory allocation failed\n");
		return -1;
	}
	memcpy( streaming_ctrl->buffer, buffer, size );
	streaming_ctrl->buffer_size = size;
	return size;
}

int
nop_streaming_read( int fd, char *buffer, int size, streaming_ctrl_t *stream_ctrl ) {
	int len=0;
//printf("nop_streaming_read\n");
	if( stream_ctrl->buffer_size!=0 ) {
		int buffer_len = stream_ctrl->buffer_size-stream_ctrl->buffer_pos;
printf("%d bytes in buffer\n", stream_ctrl->buffer_size);
		len = (size<buffer_len)?size:buffer_len;
		memcpy( buffer, (stream_ctrl->buffer)+(stream_ctrl->buffer_pos), len );
		stream_ctrl->buffer_pos += len;
printf("buffer_pos = %d\n", stream_ctrl->buffer_pos );
		if( stream_ctrl->buffer_pos>=stream_ctrl->buffer_size ) {
			free( stream_ctrl->buffer );
			stream_ctrl->buffer = NULL;
			stream_ctrl->buffer_size = 0;
			stream_ctrl->buffer_pos = 0;
printf("buffer cleaned\n");
		}
printf("read %d bytes from buffer\n", len );
	}

	if( len<size ) {
		int ret;
		ret = read( fd, buffer+len, size-len );
		if( ret<0 ) {
			printf("nop_streaming_read error : %s\n",strerror(errno));
		}
		len += ret;
//printf("read %d bytes from network\n", len );
	}
	
	return len;
}

int
nop_streaming_seek( int fd, off_t pos, streaming_ctrl_t *stream_ctrl ) {
	return -1;
}

int
nop_streaming_start( stream_t *stream ) {
	HTTP_header_t *http_hdr;
	int fd;
	if( stream==NULL ) return -1;

	fd = stream->fd;
	if( fd<0 ) {
		fd = http_send_request( stream->streaming_ctrl->url ); 
		if( fd<0 ) return -1;
		http_hdr = http_read_response( fd );
		if( http_hdr==NULL ) return -1;

		switch( http_hdr->status_code ) {
			case 200: // OK
				printf("Content-Type: [%s]\n", http_get_field(http_hdr, "Content-Type") );
				printf("Content-Length: [%s]\n", http_get_field(http_hdr, "Content-Length") );
				if( http_hdr->body_size>0 ) {
					if( streaming_bufferize( stream->streaming_ctrl, http_hdr->body, http_hdr->body_size )<0 ) {
						http_free( http_hdr );
						return -1;
					}
				}
				break;
			default:
				printf("Server return %d: %s\n", http_hdr->status_code, http_hdr->reason_phrase );
				close( fd );
				fd = -1;
		}
		stream->fd = fd;
		http_free( http_hdr );
	}

	stream->streaming_ctrl->streaming_read = nop_streaming_read;
	stream->streaming_ctrl->streaming_seek = nop_streaming_seek;
	stream->streaming_ctrl->prebuffer_size = 180000;
//	stream->streaming_ctrl->prebuffer_size = 0;
	stream->streaming_ctrl->buffering = 1;
//	stream->streaming_ctrl->buffering = 0;
	stream->streaming_ctrl->status = streaming_playing_e;
	return fd;
}

// Start listening on a UDP port. If multicast, join the group.
int
rtp_open_socket( URL_t *url ) {
	int fd;
	int socket_server_fd;
	int err, err_len;
	fd_set set;
	struct timeval tv;
	struct sockaddr_in server_address;
	struct ip_mreq mcast;

	printf("Listening for traffic on %s:%d ...\n", url->hostname, url->port );

	socket_server_fd = socket(AF_INET, SOCK_DGRAM, 0);
//	fcntl( socket_server_fd, F_SETFL, fcntl(socket_server_fd, F_GETFL) | O_NONBLOCK );
	if( socket_server_fd==-1 ) {
		perror("Failed to create socket");
		return -1;
	}

	if( isalpha(url->hostname[0]) ) {
		struct hostent *hp =(struct hostent*)gethostbyname( url->hostname );
		if( hp==NULL ) {
			printf("Counldn't resolve name: %s\n", url->hostname);
			return -1;
		}
		memcpy( (void*)&server_address.sin_addr.s_addr, (void*)hp->h_addr, hp->h_length );
	} else {
		inet_pton(AF_INET, url->hostname, &server_address.sin_addr);
	}
	server_address.sin_family=AF_INET;
	server_address.sin_port=htons(url->port);

	if( bind( socket_server_fd, (struct sockaddr*)&server_address, sizeof(server_address) )==-1 ) {
		if( errno!=EINPROGRESS ) {
			perror("Failed to connect to server");
			close(socket_server_fd);
			return -1;
		}
	}
	if((ntohl(server_address.sin_addr.s_addr) >> 28) == 0xe) {
		mcast.imr_multiaddr.s_addr = server_address.sin_addr.s_addr;
		//mcast.imr_interface.s_addr = inet_addr("10.1.1.2");
		mcast.imr_interface.s_addr = 0;
		if( setsockopt( socket_server_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mcast, sizeof(mcast))) {
			perror("IP_ADD_MEMBERSHIP failed (do you have multicasting enabled in your kernel?)");
			return -1;
		}
	}

	//tv.tv_sec = 0;
	//tv.tv_usec = (10 * 1000000);	// 10 seconds timeout
	FD_ZERO( &set );
	FD_SET( socket_server_fd, &set );
	//if( select(socket_server_fd+1, &set, NULL, NULL, &tv)>0 ) {
	if( select(socket_server_fd+1, &set, NULL, NULL, NULL)>0 ) {
		err_len = sizeof( err );
		getsockopt( socket_server_fd, SOL_SOCKET, SO_ERROR, &err, &err_len );
		if( err ) {
			printf("Timeout! No data from host %s\n", url->hostname );
			printf("Socket error: %d\n", err );
			close(socket_server_fd);
			return -1;
		}
	}
	return socket_server_fd;
}

int
rtp_streaming_read( int fd, char *buffer, int size, streaming_ctrl_t *streaming_ctrl ) {
    return read_rtp_from_server( fd, buffer, size );
}

int
rtp_streaming_start( stream_t *stream ) {
	streaming_ctrl_t *streaming_ctrl;
	int fd;

	if( stream==NULL ) return -1;
	streaming_ctrl = stream->streaming_ctrl;
	fd = stream->fd;
	
	if( fd<0 ) {
		fd = rtp_open_socket( (streaming_ctrl->url) ); 
		if( fd<0 ) return -1;
	}

	streaming_ctrl->streaming_read = rtp_streaming_read;
	streaming_ctrl->prebuffer_size = 180000;
	streaming_ctrl->buffering = 0; //1;
	streaming_ctrl->status = streaming_playing_e;
	return fd;
}

int
streaming_start(stream_t *stream, URL_t *url, int demuxer_type) {
	int ret=-1;
	if( stream==NULL ) return -1;
	
	stream->streaming_ctrl = streaming_ctrl_new( ); 
	if( stream->streaming_ctrl==NULL ) {
		return -1;
	}
	
	stream->streaming_ctrl->url = url_copy(url);
//	stream->streaming_ctrl->demuxer_type = demuxer_type;
	stream->fd = -1;

	// For RTP streams, we usually don't know the stream type until we open it.
	if( !strcmp( url->protocol, "rtp"))
	{
		stream->fd = rtp_streaming_start( stream );
	}
	// For connection-oriented streams, we can usually determine the streaming type.
	else
	switch( demuxer_type ) {
		case DEMUXER_TYPE_ASF:
			// Send the appropriate HTTP request
			// Need to filter the network stream.
			// ASF raw stream is encapsulated.
			ret = asf_streaming_start( stream );
			if( ret<0 ) {
				printf("asf_streaming_start failed\n");
			}
			break;
		case DEMUXER_TYPE_AVI:
		case DEMUXER_TYPE_MOV:
		case DEMUXER_TYPE_MPEG_ES:
		case DEMUXER_TYPE_MPEG_PS:
		case DEMUXER_TYPE_UNKNOWN:
			// Generic start, doesn't need to filter
			// the network stream, it's a raw stream
			ret = nop_streaming_start( stream );
			if( ret<0 ) {
				printf("asf_streaming_start failed\n");
			}
			break;
		default:
			printf("Unable to detect the streaming type\n");
			ret = -1;
	}

	if( ret<0 ) {
		free( stream->streaming_ctrl );
	} 
	return ret;
}

int
streaming_stop( stream_t *stream ) {
	stream->streaming_ctrl->status = streaming_stopped_e;
	return 0;
}
