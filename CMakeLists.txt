#
# mod-profession-loot-party
#
# Independent profession loot rolls and skill-up attempts for
# party/raid members.
#
# The header lives next to the source file, so no extra include
# directory is required.
#

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/ProfessionLootParty.cpp")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod-profession-loot-party.conf.dist")
