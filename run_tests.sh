#!/bin/sh

HOST=${HTTPBIN_HOST:-'httpbin.org'}
FAILURES=0

function main {
	runtest /robots.txt assert_robots
	runtest /redirect-to?url=http://$HOST/robots.txt assert_robots
	runtest /absolute-redirect/7 assert_redirect_target
	runtest /absolute-redirect/8 assert_failed_request # more than 7 redirects
	runtest /image/png assert_png
	runtest /user-agent assert_user_agent
	runtest /status/404 assert_failed_request
	runtest /status/500 assert_failed_request
	runtest /status/101 assert_failed_request # Unexpected
	runtest /status/204 assert_no_content

	# Not implemented:
	#runtest /relative-redirect/1 assert_redirect_target
}

function assert_robots {
	grep -q "User-agent:" http.out || return 1
	grep -q "Disallow:" http.out || return 1
}

function assert_user_agent {
	echo '{"user-agent":"http_client/0.1"}' | cmp - http.out || return 1
}

function assert_redirect_target {
	grep -q "http_client/0.1" http.out || return 1
}

function assert_failed_request {
	# This request must fail (retval $1 != 0),
	[[ $1 -ne 0 ]] || return 1
}

function assert_no_content {
	[[ $1 -eq 0 ]] || return 1 # HTTP 204 must be successful
	[[ -f http.out ]] && return 1 # However, there was no content to save
	return 0
}

function assert_png {
	grep -q PNG http.out || return 1
}

function runtest {
	relativeUrl=$1
	testFunction=$2

	rm -f http.out
	./http_client http://${HOST}${relativeUrl}
	retval=$?

	$testFunction $retval

	if [ $? -ne 0 ]; then
		echo "run_tests: ERROR: request ${relativeUrl} produced unexpected result." >&2
		(( FAILURES ++ ))
	fi

	echo "run_tests: test passed: ${relativeUrl}" >&2
}

main

if [ $FAILURES -ne 0 ]; then
	echo "run_tests: Number of failed tests: $FAILURES." >&2
	exit 1
fi

echo "run_tests: all tests passed." >&2
