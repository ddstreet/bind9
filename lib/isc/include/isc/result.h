
#ifndef ISC_RESULT_H
#define ISC_RESULT_H 1

typedef unsigned int isc_result_t;

#define ISC_R_SUCCESS			0
#define ISC_R_NOMEMORY			1
#define ISC_R_TIMEDOUT			2
#define ISC_R_NOTHREADS			3
#define ISC_R_ADDRNOTAVAIL		4
#define ISC_R_ADDRINUSE			5
#define ISC_R_NOPERM			6
#define ISC_R_NOCONN			7
#define ISC_R_NETUNREACH		8
#define ISC_R_HOSTUNREACH		9
#define ISC_R_NETDOWN			10
#define ISC_R_HOSTDOWN			11
#define ISC_R_CONNREFUSED		12
#define ISC_R_UNEXPECTED		0xFFFFFFFFL

#define isc_result_totext		__isc_result_totext

char *					isc_result_totext(isc_result_t);

#endif /* ISC_RESULT_H */
