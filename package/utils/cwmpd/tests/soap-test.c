// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/model.h"
#include "../src/soap.h"

static unsigned int checks;
#define CHECK(v) do { assert(v); checks++; } while (0)

static const char gpv[] =
	"<?xml version=\"1.0\"?>\n"
	"<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\""
	" xmlns:cwmp=\"urn:dslforum-org:cwmp-1-2\">"
	"<SOAP-ENV:Header><cwmp:ID SOAP-ENV:mustUnderstand=\"1\">id&amp;7</cwmp:ID>"
	"</SOAP-ENV:Header><SOAP-ENV:Body><cwmp:GetParameterValues>"
	"<ParameterNames SOAP-ENC:arrayType=\"xsd:string[2]\">"
	"<string>Device.DeviceInfo.SerialNumber</string><string>Device.ManagementServer.</string>"
	"</ParameterNames></cwmp:GetParameterValues></SOAP-ENV:Body></SOAP-ENV:Envelope>";

static const char gpn[] =
	"<s:Envelope xmlns:s=\"x\" xmlns:c=\"urn:dslforum-org:cwmp-1-0\"><s:Header>"
	"<c:ID>9</c:ID></s:Header><s:Body><c:GetParameterNames>"
	"<ParameterPath>Device.</ParameterPath><NextLevel>true</NextLevel>"
	"</c:GetParameterNames></s:Body></s:Envelope>";

static const char spv[] =
	"<Envelope><Header><ID>3</ID></Header><Body><cwmp:SetParameterValues>"
	"<ParameterList>"
	"<ParameterValueStruct><Name>Device.X.Password</Name><Value>top&amp;1</Value></ParameterValueStruct>"
	"<ParameterValueStruct><Name>Device.X.Empty</Name><Value xsi:type=\"xsd:string\"/></ParameterValueStruct>"
	"</ParameterList>"
	"<ParameterKey>k42</ParameterKey>"
	"</cwmp:SetParameterValues></Body></Envelope>";

static const char dl[] =
	"<Envelope><Header><ID>4</ID></Header><Body><cwmp:Download>"
	"<CommandKey>fw1</CommandKey><FileType>1 Firmware Upgrade Image</FileType>"
	"<URL>http://acs/fw.bin?a=1&amp;b=2</URL><Username>u</Username>"
	"<Password>p&lt;w</Password><FileSize>1048576</FileSize>"
	"<TargetFileName>fw.bin</TargetFileName><DelaySeconds>5</DelaySeconds>"
	"</cwmp:Download></Body></Envelope>";

static const char spa[] =
	"<Envelope><Header><ID>5</ID></Header><Body><cwmp:SetParameterAttributes>"
	"<ParameterList>"
	"<SetParameterAttributesStruct><Name>Device.X.Notify</Name>"
	"<NotificationChange>1</NotificationChange><Notification>2</Notification>"
	"<AccessListChange>true</AccessListChange>"
	"<AccessList><string>Subscriber</string></AccessList>"
	"</SetParameterAttributesStruct>"
	"</ParameterList></cwmp:SetParameterAttributes></Body></Envelope>";

int main(void)
{
	struct soap_request req;
#define MAX 128
	struct soap_param out[MAX];
	const char *names[MAX * 2];
	bool writable[MAX * 2];
	char *owned[MAX * 2] = { 0 };
	struct model m = { 0 };
	struct buf b = { 0 };
	int n, i;

	/* An empty body ends the ACS's turn; the default namespace is kept */
	CHECK(!soap_parse("", 0, &req));
	CHECK(!strcmp(req.cwmp_ns, CWMP_NS_DEFAULT));

	CHECK(soap_parse(gpv, strlen(gpv), &req));
	CHECK(!strcmp(req.rpc, "GetParameterValues") && !req.fault);
	CHECK(!strcmp(req.id, "id&7"));
	CHECK(!strcmp(req.cwmp_ns, "urn:dslforum-org:cwmp-1-2"));
	CHECK(req.n_names == 2 && !strcmp(req.names[1], "Device.ManagementServer."));

	model_set(&m, "Device.DeviceInfo.SerialNumber", "SN<1>", NULL);
	model_set(&m, "Device.DeviceInfo.Manufacturer", "VSOL", NULL);
	model_set(&m, "Device.ManagementServer.URL", "https://acs", NULL);
	model_set(&m, "Device.ManagementServer.ParameterKey", "", NULL);
	model_set(&m, "Device.RootDataModelVersion", "2.11", NULL);

	/* an exact name and a partial path */
	n = model_values(&m, req.names, req.n_names, out, MAX);
	CHECK(n == 3);
	CHECK(soap_get_parameter_values(&b, &req, out, n));
	CHECK(strstr(b.data, "<cwmp:ID soap-env:mustUnderstand=\"1\">id&amp;7</cwmp:ID>"));
	CHECK(strstr(b.data, "xmlns:cwmp=\"urn:dslforum-org:cwmp-1-2\""));
	CHECK(strstr(b.data, "<Value xsi:type=\"xsd:string\">SN&lt;1&gt;</Value>"));
	CHECK(strstr(b.data, "ParameterValueStruct[3]"));
	buf_free(&b);

	/* an unknown name is CWMP fault 9005 */
	{
		char *bad[] = { "Device.Nope" };

		CHECK(model_values(&m, bad, 1, out, MAX) == -1);
		CHECK(soap_fault(&b, &req, 9005, "Invalid parameter name"));
		CHECK(strstr(b.data, "<FaultCode>9005</FaultCode>"));
		buf_free(&b);
	}
	soap_request_free(&req);

	/* NextLevel: the direct children only */
	CHECK(soap_parse(gpn, strlen(gpn), &req));
	CHECK(!strcmp(req.rpc, "GetParameterNames") && req.next_level);
	CHECK(!strcmp(req.path, "Device.") && !strcmp(req.id, "9"));
	n = model_names(&m, req.path, req.next_level, names, MAX * 2, owned);
	CHECK(n == 3);
	for (i = 0; i < n; i++)
		CHECK(!strcmp(names[i], "Device.DeviceInfo.") ||
		      !strcmp(names[i], "Device.ManagementServer.") ||
		      !strcmp(names[i], "Device.RootDataModelVersion"));
	for (i = 0; i < MAX * 2; i++)
		free(owned[i]), owned[i] = NULL;

	/* the full subtree names the object asked for and every parameter */
	n = model_names(&m, "Device.DeviceInfo.", false, names, MAX * 2, owned);
	CHECK(n == 3 && !strcmp(names[0], "Device.DeviceInfo."));
	for (i = 0; i < n; i++)
		writable[i] = model_writable(&m, names[i]);
	CHECK(soap_get_parameter_names(&b, &req, names, writable, n));
	CHECK(strstr(b.data, "<Name>Device.DeviceInfo.SerialNumber</Name><Writable>0</Writable>"));
	buf_free(&b);
	for (i = 0; i < MAX * 2; i++)
		free(owned[i]), owned[i] = NULL;
	CHECK(model_names(&m, "Device.Nope.", false, names, MAX * 2, owned) == -1);
	CHECK(model_names(&m, "Device.RootDataModelVersion", true, names, MAX * 2, owned) == -2);
	soap_request_free(&req);

	/* SetParameterValues is parsed for capture and acknowledged with Status 0 */
	CHECK(soap_parse(spv, strlen(spv), &req));
	CHECK(!strcmp(req.rpc, "SetParameterValues") && !strcmp(req.id, "3"));
	CHECK(req.has_param_key && !strcmp(req.param_key, "k42"));
	CHECK(req.n_set == 2);
	CHECK(!strcmp(req.set_params[0].name, "Device.X.Password") &&
	      !strcmp(req.set_params[0].value, "top&1"));
	CHECK(!strcmp(req.set_params[1].name, "Device.X.Empty") &&
	      !strcmp(req.set_params[1].value, ""));
	CHECK(soap_empty_response(&b, &req, "SetParameterValuesResponse", "<Status>0</Status>"));
	CHECK(strstr(b.data, "<cwmp:SetParameterValuesResponse><Status>0</Status></cwmp:SetParameterValuesResponse>"));
	buf_free(&b);
	soap_request_free(&req);

	/* a Download is read in full, for the fetch and the TransferComplete */
	CHECK(soap_parse(dl, strlen(dl), &req));
	CHECK(!strcmp(req.rpc, "Download") && !strcmp(req.id, "4"));
	CHECK(!strcmp(req.command_key, "fw1"));
	CHECK(!strcmp(req.file_type, "1 Firmware Upgrade Image"));
	CHECK(!strcmp(req.url, "http://acs/fw.bin?a=1&b=2"));
	CHECK(!strcmp(req.username, "u") && !strcmp(req.password, "p<w"));
	CHECK(req.file_size == 1048576 && req.delay == 5 && !strcmp(req.target, "fw.bin"));
	soap_request_free(&req);
	CHECK(soap_transfer_complete(&b, "8", "fw1", 0, "x", "2026-01-01T00:00:00Z",
				     "2026-01-01T00:00:09Z"));
	CHECK(strstr(b.data, "<cwmp:TransferComplete><CommandKey>fw1</CommandKey>"
		     "<FaultStruct><FaultCode>0</FaultCode><FaultString></FaultString>"));
	CHECK(strstr(b.data, "<CompleteTime>2026-01-01T00:00:09Z</CompleteTime>"));
	buf_free(&b);
	CHECK(soap_transfer_complete(&b, "9", "fw1", 9010, "Download failure", "a", "b"));
	CHECK(strstr(b.data, "<FaultCode>9010</FaultCode><FaultString>Download failure"));
	buf_free(&b);

	/* values the ACS set live in their own table; DeleteObject drops a subtree */
	{
		struct model sh = { 0 };
		char *obj[] = { "Device.X.1." };

		model_set(&sh, "Device.X.1.A", "a", NULL);
		model_set(&sh, "Device.X.1.B", "b", NULL);
		model_set(&sh, "Device.X.2.A", "c", NULL);
		model_set(&sh, "Device.X.3.", "", NULL);
		CHECK(model_exists(&sh, "Device.X.3."));
		CHECK(model_writable(&sh, "Device.X."));
		CHECK(model_writable(&sh, "Device.X.3."));
		CHECK(model_writable(&sh, "Device.X.1.A"));
		CHECK(!model_writable(&m, "Device.ManagementServer.ParameterKey"));
		CHECK(model_values(&sh, (char *[]){ "Device.X.3." }, 1, out, MAX) == 0);
		CHECK(model_names(&sh, "Device.X.3.", true, names, MAX * 2, owned) == 0);
		CHECK(model_names(&sh, "Device.X.3.", false, names, MAX * 2, owned) == 1);
		for (i = 0; i < MAX * 2; i++)
			free(owned[i]), owned[i] = NULL;
		CHECK(model_values(&sh, obj, 1, out, MAX) == 2);
		model_del(&sh, "Device.X.1.");
		CHECK(sh.n == 2 && !strcmp(model_get(&sh, "Device.X.2.A"), "c"));
		CHECK(model_values(&sh, obj, 1, out, MAX) == -1);
		model_free(&sh);
	}

	/* SetParameterAttributes + GetParameterAttributes response */
	CHECK(soap_parse(spa, strlen(spa), &req));
	CHECK(!strcmp(req.rpc, "SetParameterAttributes") && req.n_attrs == 1);
	CHECK(!strcmp(req.set_attrs[0].name, "Device.X.Notify"));
	CHECK(req.set_attrs[0].notif_change && req.set_attrs[0].notification == 2);
	CHECK(req.set_attrs[0].access_change && !strcmp(req.set_attrs[0].access_list, "Subscriber"));
	{
		struct soap_attr a = { "Device.X.Notify", 2, "Subscriber" };

		CHECK(soap_get_parameter_attributes(&b, &req, &a, 1));
		CHECK(strstr(b.data, "<Notification>2</Notification>"));
		CHECK(strstr(b.data, "<string>Subscriber</string>"));
		buf_free(&b);
	}
	soap_request_free(&req);

	/* numeric entities, CDATA, and >64 parameter names in GetParameterValues */
	{
		struct buf big = { 0 };
		static const char cdata_spv[] =
			"<Envelope><Header><ID>6</ID></Header><Body><cwmp:SetParameterValues>"
			"<ParameterList>"
			"<ParameterValueStruct><Name>Device.X.C</Name>"
			"<Value><![CDATA[a<b&c]]></Value></ParameterValueStruct>"
			"<ParameterValueStruct><Name>Device.X.N</Name>"
			"<Value>&#65;&#x26;B</Value></ParameterValueStruct>"
			"</ParameterList><ParameterKey/></cwmp:SetParameterValues></Body></Envelope>";

		CHECK(soap_parse(cdata_spv, strlen(cdata_spv), &req));
		CHECK(req.n_set == 2 && !strcmp(req.set_params[0].value, "a<b&c"));
		CHECK(!strcmp(req.set_params[1].value, "A&B"));
		soap_request_free(&req);

		buf_printf(&big, "<Envelope><Header><ID>7</ID></Header><Body>"
			   "<cwmp:GetParameterValues><ParameterNames>");
		for (i = 0; i < 80; i++)
			buf_printf(&big, "<string>Device.P.%d</string>", i);
		buf_printf(&big, "</ParameterNames></cwmp:GetParameterValues></Body></Envelope>");
		CHECK(soap_parse(big.data, big.len, &req));
		CHECK(req.n_names == 80 && !strcmp(req.names[79], "Device.P.79"));
		soap_request_free(&req);
		buf_free(&big);
	}

	/* the Inform carries the device, its events and parameters */
	{
		struct soap_device_id dev = { "VSOL", "AABBCC", "HG325AX30", "VSOL&1" };
		const struct soap_event events[] = {
			{ "0 BOOTSTRAP", "" }, { "M Reboot", "r<1" },
		};

		CHECK(soap_inform(&b, "1", &dev, events, 2, 0, "2026-01-01T00:00:00Z", m.p, m.n));
		CHECK(strstr(b.data, "<EventCode>M Reboot</EventCode><CommandKey>r&lt;1</CommandKey>"));
		CHECK(strstr(b.data, "<SerialNumber>VSOL&amp;1</SerialNumber>"));
		CHECK(strstr(b.data, "cwmp:EventStruct[2]"));
		CHECK(strstr(b.data, "<EventCode>0 BOOTSTRAP</EventCode>"));
		CHECK(strstr(b.data, "ParameterValueStruct[5]"));
		buf_free(&b);
	}

	model_free(&m);
	printf("%u CWMP SOAP and data model checks passed\n", checks);
	return 0;
}
