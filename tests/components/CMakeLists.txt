add_executable(tcomponents tcomponents.cc)
target_link_libraries(tcomponents
	askap_components
	${CPPUNIT_LIBRARY}
)
add_test(
	NAME tcomponents
	COMMAND tcomponents
	)
