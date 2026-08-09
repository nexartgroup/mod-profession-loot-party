#
# mod-profession-loot-party
#
# Independent profession loot rolls for party/raid members.
#

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/ProfessionLootParty.cpp")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod-profession-loot-party.conf.dist")